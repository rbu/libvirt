/*
 * nwfilter_ebiptables_driver.c: driver for ebtables/iptables on tap devices
 *
 * Copyright (C) 2010 IBM Corp.
 * Copyright (C) 2010 Stefan Berger
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Author: Stefan Berger <stefanb@us.ibm.com>
 */

#include <config.h>

#include <sys/stat.h>

#include "internal.h"

#include "buf.h"
#include "memory.h"
#include "logging.h"
#include "virterror_internal.h"
#include "domain_conf.h"
#include "nwfilter_gentech_driver.h"
#include "nwfilter_ebiptables_driver.h"


#define VIR_FROM_THIS VIR_FROM_NWFILTER


#define EBTABLES_DEFAULT_TABLE  "nat"
#define EBTABLES_CHAIN_INCOMING "PREROUTING"
#define EBTABLES_CHAIN_OUTGOING "POSTROUTING"

#define CHAINPREFIX_HOST_IN       'I'
#define CHAINPREFIX_HOST_OUT      'O'
#define CHAINPREFIX_HOST_IN_TEMP  'J'
#define CHAINPREFIX_HOST_OUT_TEMP 'P'


#define CMD_SEPARATOR "\n"
#define CMD_DEF_PRE  "cmd=\""
#define CMD_DEF_POST "\""
#define CMD_DEF(X) CMD_DEF_PRE X CMD_DEF_POST
#define CMD_EXEC   "res=`${cmd}`" CMD_SEPARATOR
#define CMD_STOPONERR(X) \
    X ? "if [ $? -ne 0 ]; then" \
        "  echo \"Failure to execute command '${cmd}'.\";" \
        "  exit 1;" \
        "fi" CMD_SEPARATOR \
      : ""


#define EBTABLES_CMD  EBTABLES_PATH
#define IPTABLES_CMD  IPTABLES_PATH
#define IP6TABLES_CMD IP6TABLES_PATH
#define BASH_CMD      BASH_PATH
#define GREP_CMD      GREP_PATH
#define GAWK_CMD      GAWK_PATH

#define PRINT_ROOT_CHAIN(buf, prefix, ifname) \
    snprintf(buf, sizeof(buf), "libvirt-%c-%s", prefix, ifname)
#define PRINT_CHAIN(buf, prefix, ifname, suffix) \
    snprintf(buf, sizeof(buf), "%c-%s-%s", prefix, ifname, suffix)


#define VIRT_IN_CHAIN      "libvirt-in"
#define VIRT_OUT_CHAIN     "libvirt-out"
#define VIRT_IN_POST_CHAIN "libvirt-in-post"
#define HOST_IN_CHAIN      "libvirt-host-in"

#define PRINT_IPT_ROOT_CHAIN(buf, prefix, ifname) \
    snprintf(buf, sizeof(buf), "%c%c-%s", prefix[0], prefix[1], ifname)

#define PHYSDEV_IN  "--physdev-in"
#define PHYSDEV_OUT "--physdev-out"

static const char *m_state_out_str   = "-m state --state NEW,ESTABLISHED";
static const char *m_state_in_str    = "-m state --state ESTABLISHED";
static const char *m_physdev_in_str  = "-m physdev " PHYSDEV_IN;
static const char *m_physdev_out_str = "-m physdev " PHYSDEV_OUT;

#define MATCH_STATE_OUT    m_state_out_str
#define MATCH_STATE_IN     m_state_in_str
#define MATCH_PHYSDEV_IN   m_physdev_in_str
#define MATCH_PHYSDEV_OUT  m_physdev_out_str


static const char *supported_protocols[] = {
    "ipv4",
    "ipv6",
    "arp",
    NULL,
};


static int
printVar(virConnectPtr conn,
         virNWFilterHashTablePtr vars,
         char *buf, int bufsize,
         nwItemDescPtr item,
         int *done)
{
    *done = 0;

    if ((item->flags & NWFILTER_ENTRY_ITEM_FLAG_HAS_VAR)) {
        char *val = (char *)virHashLookup(vars->hashTable, item->var);
        if (!val) {
            virNWFilterReportError(conn, VIR_ERR_INVALID_NWFILTER,
                                   _("cannot find value for '%s'"),
                                   item->var);
            return 1;
        }

        if (!virStrcpy(buf, val, bufsize)) {
            virNWFilterReportError(conn, VIR_ERR_INVALID_NWFILTER,
                                   _("Buffer to small to print MAC address "
                                   "'%s' into"),
                                   item->var);
            return 1;
        }

        *done = 1;
    }
    return 0;
}


static int
printDataType(virConnectPtr conn,
              virNWFilterHashTablePtr vars,
              char *buf, int bufsize,
              nwItemDescPtr item)
{
    int done;
    char *data;

    if (printVar(conn, vars, buf, bufsize, item, &done))
        return 1;

    if (done)
        return 0;

    switch (item->datatype) {
    case DATATYPE_IPADDR:
        data = virSocketFormatAddr(&item->u.ipaddr.addr);
        if (!data) {
            virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                                   _("internal IPv4 address representation "
                                     "is bad"));
            return 1;
        }
        if (snprintf(buf, bufsize, "%s", data) >= bufsize) {
            virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                                   _("buffer too small for IP address"));
            VIR_FREE(data);
            return 1;
        }
        VIR_FREE(data);
    break;

    case DATATYPE_IPV6ADDR:
        data = virSocketFormatAddr(&item->u.ipaddr.addr);
        if (!data) {
            virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                                   _("internal IPv6 address representation "
                                     "is bad"));
            return 1;
        }

        if (snprintf(buf, bufsize, "%s", data) >= bufsize) {
            virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                                   _("buffer too small for IPv6 address"));
            VIR_FREE(data);
            return 1;
        }
        VIR_FREE(data);
    break;

    case DATATYPE_MACADDR:
    case DATATYPE_MACMASK:
        if (bufsize < VIR_MAC_STRING_BUFLEN) {
            virNWFilterReportError(conn, VIR_ERR_INVALID_NWFILTER,
                                   _("Buffer too small for MAC address"));
            return 1;
        }

        virFormatMacAddr(item->u.macaddr.addr, buf);
    break;

    case DATATYPE_UINT16:
        if (snprintf(buf, bufsize, "%d",
                     item->u.u16) >= bufsize) {
            virNWFilterReportError(conn, VIR_ERR_INVALID_NWFILTER,
                                   _("Buffer too small for uint16 type"));
            return 1;
        }
    break;

    case DATATYPE_IPV6MASK:
    case DATATYPE_IPMASK:
    case DATATYPE_UINT8:
        if (snprintf(buf, bufsize, "%d",
                     item->u.u8) >= bufsize) {
            virNWFilterReportError(conn, VIR_ERR_INVALID_NWFILTER,
                                   _("Buffer too small for uint8 type"));
            return 1;
        }
    break;

    default:
        virNWFilterReportError(conn, VIR_ERR_INVALID_NWFILTER,
                               _("Unhandled datatype %x"), item->datatype);
        return 1;
    break;
    }

    return 0;
}


static void
ebiptablesRuleInstFree(ebiptablesRuleInstPtr inst)
{
    if (!inst)
        return;

    VIR_FREE(inst->commandTemplate);
    VIR_FREE(inst);
}


static int
ebiptablesAddRuleInst(virConnectPtr conn,
                      virNWFilterRuleInstPtr res,
                      char *commandTemplate,
                      enum virNWFilterChainSuffixType neededChain,
                      char chainprefix,
                      unsigned int priority,
                      enum RuleType ruleType)
{
    ebiptablesRuleInstPtr inst;

    if (VIR_ALLOC(inst) < 0) {
        virReportOOMError();
        return 1;
    }

    inst->commandTemplate = commandTemplate;
    inst->neededProtocolChain = neededChain;
    inst->chainprefix = chainprefix;
    inst->priority = priority;
    inst->ruleType = ruleType;

    return virNWFilterRuleInstAddData(conn, res, inst);
}


static int
ebtablesHandleEthHdr(virConnectPtr conn,
                     virBufferPtr buf,
                     virNWFilterHashTablePtr vars,
                     ethHdrDataDefPtr ethHdr)
{
    char macaddr[VIR_MAC_STRING_BUFLEN];

    if (HAS_ENTRY_ITEM(&ethHdr->dataSrcMACAddr)) {
        if (printDataType(conn,
                          vars,
                          macaddr, sizeof(macaddr),
                          &ethHdr->dataSrcMACAddr))
            goto err_exit;

        virBufferVSprintf(buf,
                      " -s %s %s",
                      ENTRY_GET_NEG_SIGN(&ethHdr->dataSrcMACAddr),
                      macaddr);

        if (HAS_ENTRY_ITEM(&ethHdr->dataSrcMACMask)) {
            if (printDataType(conn,
                              vars,
                              macaddr, sizeof(macaddr),
                              &ethHdr->dataSrcMACMask))
                goto err_exit;

            virBufferVSprintf(buf,
                              "/%s",
                              macaddr);
        }
    }

    if (HAS_ENTRY_ITEM(&ethHdr->dataDstMACAddr)) {
        if (printDataType(conn,
                          vars,
                          macaddr, sizeof(macaddr),
                          &ethHdr->dataDstMACAddr))
            goto err_exit;

        virBufferVSprintf(buf,
                      " -d %s %s",
                      ENTRY_GET_NEG_SIGN(&ethHdr->dataDstMACAddr),
                      macaddr);

        if (HAS_ENTRY_ITEM(&ethHdr->dataDstMACMask)) {
            if (printDataType(conn,
                              vars,
                              macaddr, sizeof(macaddr),
                              &ethHdr->dataDstMACMask))
                goto err_exit;

            virBufferVSprintf(buf,
                              "/%s",
                              macaddr);
        }
    }

    return 0;

 err_exit:
    virBufferFreeAndReset(buf);

    return 1;
}


/************************ iptables support ************************/

static int iptablesLinkIPTablesBaseChain(virConnectPtr conn ATTRIBUTE_UNUSED,
                                         const char *iptables_cmd,
                                         virBufferPtr buf,
                                         const char *udchain,
                                         const char *syschain,
                                         unsigned int pos,
                                         int stopOnError)
{
    virBufferVSprintf(buf,
                      "res=$(%s -L %s -n --line-number | "
                          GREP_CMD " \" %s \")\n"
                      "if [ $? -ne 0 ]; then\n"
                      "  %s -I %s %d -j %s\n"
                      "else\n"
                      "  r=$(echo $res | " GAWK_CMD " '{print $1}')\n"
                      "  if [ \"${r}\" != \"%d\" ]; then\n"
                      "    " CMD_DEF("%s -I %s %d -j %s") CMD_SEPARATOR
                      "    " CMD_EXEC
                      "    %s"
                      "    let r=r+1\n"
                      "    " CMD_DEF("%s -D %s ${r}") CMD_SEPARATOR
                      "    " CMD_EXEC
                      "    %s"
                      "  fi\n"
                      "fi\n",

                      iptables_cmd, syschain,
                      udchain,

                      iptables_cmd, syschain, pos, udchain,

                      pos,

                      iptables_cmd, syschain, pos, udchain,
                      CMD_STOPONERR(stopOnError),

                      iptables_cmd, syschain,
                      CMD_STOPONERR(stopOnError));
    return 0;
}


static int iptablesCreateBaseChains(virConnectPtr conn,
                                    const char *iptables_cmd,
                                    virBufferPtr buf)
{
    virBufferVSprintf(buf,"%s -N " VIRT_IN_CHAIN      CMD_SEPARATOR
                          "%s -N " VIRT_OUT_CHAIN     CMD_SEPARATOR
                          "%s -N " VIRT_IN_POST_CHAIN CMD_SEPARATOR
                          "%s -N " HOST_IN_CHAIN      CMD_SEPARATOR,
                          iptables_cmd,
                          iptables_cmd,
                          iptables_cmd,
                          iptables_cmd);
    iptablesLinkIPTablesBaseChain(conn, iptables_cmd, buf,
                                  VIRT_IN_CHAIN     , "FORWARD", 1, 1);
    iptablesLinkIPTablesBaseChain(conn, iptables_cmd, buf,
                                  VIRT_OUT_CHAIN    , "FORWARD", 2, 1);
    iptablesLinkIPTablesBaseChain(conn, iptables_cmd, buf,
                                  VIRT_IN_POST_CHAIN, "FORWARD", 3, 1);
    iptablesLinkIPTablesBaseChain(conn, iptables_cmd, buf,
                                  HOST_IN_CHAIN     , "INPUT"  , 1, 1);

    return 0;
}


static int
iptablesCreateTmpRootChain(virConnectPtr conn ATTRIBUTE_UNUSED,
                           const char *iptables_cmd,
                           virBufferPtr buf,
                           char prefix,
                           int incoming, const char *ifname,
                           int stopOnError)
{
    char chain[MAX_CHAINNAME_LENGTH];
    char chainPrefix[2] = {
       prefix,
       (incoming) ? CHAINPREFIX_HOST_IN_TEMP
                  : CHAINPREFIX_HOST_OUT_TEMP
    };

    PRINT_IPT_ROOT_CHAIN(chain, chainPrefix, ifname);

    virBufferVSprintf(buf,
                      CMD_DEF("%s -N %s") CMD_SEPARATOR
                      CMD_EXEC
                      "%s",
                      iptables_cmd,
                      chain,
                      CMD_STOPONERR(stopOnError));

    return 0;
}


static int
iptablesCreateTmpRootChains(virConnectPtr conn,
                            const char *iptables_cmd,
                            virBufferPtr buf,
                            const char *ifname)
{
    iptablesCreateTmpRootChain(conn, iptables_cmd, buf, 'F', 0, ifname, 1);
    iptablesCreateTmpRootChain(conn, iptables_cmd, buf, 'F', 1, ifname, 1);
    iptablesCreateTmpRootChain(conn, iptables_cmd, buf, 'H', 1, ifname, 1);
    return 0;
}


static int
_iptablesRemoveRootChain(virConnectPtr conn ATTRIBUTE_UNUSED,
                         const char *iptables_cmd,
                         virBufferPtr buf,
                         char prefix,
                         int incoming, const char *ifname,
                         int isTempChain)
{
    char chain[MAX_CHAINNAME_LENGTH];
    char chainPrefix[2] = {
        prefix,
    };

    if (isTempChain)
        chainPrefix[1] = (incoming) ? CHAINPREFIX_HOST_IN_TEMP
                                    : CHAINPREFIX_HOST_OUT_TEMP;
    else
        chainPrefix[1] = (incoming) ? CHAINPREFIX_HOST_IN
                                    : CHAINPREFIX_HOST_OUT;

    PRINT_IPT_ROOT_CHAIN(chain, chainPrefix, ifname);

    virBufferVSprintf(buf,
                      "%s -F %s" CMD_SEPARATOR
                      "%s -X %s" CMD_SEPARATOR,
                      iptables_cmd, chain,
                      iptables_cmd, chain);

    return 0;
}


static int
iptablesRemoveRootChain(virConnectPtr conn ATTRIBUTE_UNUSED,
                        const char *iptables_cmd,
                        virBufferPtr buf,
                        char prefix,
                        int incoming,
                        const char *ifname)
{
    return _iptablesRemoveRootChain(conn, iptables_cmd,
                                    buf, prefix, incoming, ifname, 0);
}


static int
iptablesRemoveTmpRootChain(virConnectPtr conn ATTRIBUTE_UNUSED,
                           const char *iptables_cmd,
                           virBufferPtr buf,
                           char prefix,
                           int incoming,
                           const char *ifname)
{
    return _iptablesRemoveRootChain(conn, iptables_cmd, buf, prefix,
                                    incoming, ifname, 1);
}


static int
iptablesRemoveTmpRootChains(virConnectPtr conn,
                            const char *iptables_cmd,
                            virBufferPtr buf,
                            const char *ifname)
{
    iptablesRemoveTmpRootChain(conn, iptables_cmd, buf, 'F', 0, ifname);
    iptablesRemoveTmpRootChain(conn, iptables_cmd, buf, 'F', 1, ifname);
    iptablesRemoveTmpRootChain(conn, iptables_cmd, buf, 'H', 1, ifname);
    return 0;
}


static int
iptablesRemoveRootChains(virConnectPtr conn,
                         const char *iptables_cmd,
                         virBufferPtr buf,
                         const char *ifname)
{
    iptablesRemoveRootChain(conn, iptables_cmd, buf, 'F', 0, ifname);
    iptablesRemoveRootChain(conn, iptables_cmd, buf, 'F', 1, ifname);
    iptablesRemoveRootChain(conn, iptables_cmd, buf, 'H', 1, ifname);
    return 0;
}


static int
iptablesLinkTmpRootChain(virConnectPtr conn ATTRIBUTE_UNUSED,
                         const char *iptables_cmd,
                         virBufferPtr buf,
                         const char *basechain,
                         char prefix,
                         int incoming, const char *ifname,
                         int stopOnError)
{
    char chain[MAX_CHAINNAME_LENGTH];
    char chainPrefix[2] = {
        prefix,
        (incoming) ? CHAINPREFIX_HOST_IN_TEMP
                   : CHAINPREFIX_HOST_OUT_TEMP
    };
    const char *match = (incoming) ? MATCH_PHYSDEV_IN
                                   : MATCH_PHYSDEV_OUT;

    PRINT_IPT_ROOT_CHAIN(chain, chainPrefix, ifname);

    virBufferVSprintf(buf,
                      CMD_DEF("%s -A %s "
                              "%s %s -g %s") CMD_SEPARATOR
                      CMD_EXEC
                      "%s",
                      iptables_cmd,
                      basechain,
                      match, ifname, chain,

                      CMD_STOPONERR(stopOnError));

    return 0;
}


static int
iptablesLinkTmpRootChains(virConnectPtr conn,
                          const char *cmd,
                          virBufferPtr buf,
                          const char *ifname)
{
    iptablesLinkTmpRootChain(conn, cmd, buf, VIRT_OUT_CHAIN, 'F', 0, ifname, 1);
    iptablesLinkTmpRootChain(conn, cmd, buf, VIRT_IN_CHAIN , 'F', 1, ifname, 1);
    iptablesLinkTmpRootChain(conn, cmd, buf, HOST_IN_CHAIN , 'H', 1, ifname, 1);

    return 0;
}


static int
iptablesSetupVirtInPost(virConnectPtr conn ATTRIBUTE_UNUSED,
                        const char *iptables_cmd,
                        virBufferPtr buf,
                        const char *ifname)
{
    const char *match = MATCH_PHYSDEV_IN;
    virBufferVSprintf(buf,
                      "res=$(%s -L " VIRT_IN_POST_CHAIN
                      " | grep \"\\%s %s\")\n"
                      "if [ \"${res}\" == \"\" ]; then "
                        CMD_DEF("%s"
                        " -A " VIRT_IN_POST_CHAIN
                        " %s %s -j ACCEPT") CMD_SEPARATOR
                        CMD_EXEC
                        "%s"
                      "fi\n",
                      iptables_cmd,
                      PHYSDEV_IN, ifname,
                      iptables_cmd,
                      match, ifname,
                      CMD_STOPONERR(1));
    return 0;
}


static int
iptablesClearVirtInPost(virConnectPtr conn ATTRIBUTE_UNUSED,
                        const char *iptables_cmd,
                        virBufferPtr buf,
                        const char *ifname)
{
    const char *match = MATCH_PHYSDEV_IN;
    virBufferVSprintf(buf,
                      "%s -D " VIRT_IN_POST_CHAIN
                      " %s %s -j ACCEPT" CMD_SEPARATOR,
                      iptables_cmd,
                      match, ifname);
    return 0;
}

static int
_iptablesUnlinkRootChain(virConnectPtr conn ATTRIBUTE_UNUSED,
                        const char *iptables_cmd,
                        virBufferPtr buf,
                        const char *basechain,
                        char prefix,
                        int incoming, const char *ifname,
                        int isTempChain)
{
    char chain[MAX_CHAINNAME_LENGTH];
    char chainPrefix[2] = {
        prefix,
    };
    if (isTempChain)
        chainPrefix[1] = (incoming) ? CHAINPREFIX_HOST_IN_TEMP
                                    : CHAINPREFIX_HOST_OUT_TEMP;
    else
        chainPrefix[1] = (incoming) ? CHAINPREFIX_HOST_IN
                                    : CHAINPREFIX_HOST_OUT;
    const char *match = (incoming) ? MATCH_PHYSDEV_IN
                                   : MATCH_PHYSDEV_OUT;

    PRINT_IPT_ROOT_CHAIN(chain, chainPrefix, ifname);

    virBufferVSprintf(buf,
                      "%s -D %s "
                      "%s %s -g %s" CMD_SEPARATOR,
                      iptables_cmd,
                      basechain,
                      match, ifname, chain);

    return 0;
}


static int
iptablesUnlinkRootChain(virConnectPtr conn ATTRIBUTE_UNUSED,
                        const char *iptables_cmd,
                        virBufferPtr buf,
                        const char *basechain,
                        char prefix,
                        int incoming, const char *ifname)
{
    return _iptablesUnlinkRootChain(conn, iptables_cmd, buf,
                                    basechain, prefix, incoming, ifname, 0);
}


static int
iptablesUnlinkTmpRootChain(virConnectPtr conn ATTRIBUTE_UNUSED,
                           const char *iptables_cmd,
                           virBufferPtr buf,
                           const char *basechain,
                           char prefix,
                           int incoming, const char *ifname)
{
    return _iptablesUnlinkRootChain(conn, iptables_cmd, buf,
                                    basechain, prefix, incoming, ifname, 1);
}


static int
iptablesUnlinkRootChains(virConnectPtr conn,
                         const char *cmd,
                         virBufferPtr buf,
                         const char *ifname)
{
    iptablesUnlinkRootChain(conn, cmd, buf, VIRT_OUT_CHAIN, 'F', 0, ifname);
    iptablesUnlinkRootChain(conn, cmd, buf, VIRT_IN_CHAIN , 'F', 1, ifname);
    iptablesUnlinkRootChain(conn, cmd, buf, HOST_IN_CHAIN , 'H', 1, ifname);

    return 0;
}


static int
iptablesUnlinkTmpRootChains(virConnectPtr conn,
                            const char *cmd,
                            virBufferPtr buf,
                            const char *ifname)
{
    iptablesUnlinkTmpRootChain(conn, cmd, buf, VIRT_OUT_CHAIN, 'F', 0, ifname);
    iptablesUnlinkTmpRootChain(conn, cmd, buf, VIRT_IN_CHAIN , 'F', 1, ifname);
    iptablesUnlinkTmpRootChain(conn, cmd, buf, HOST_IN_CHAIN , 'H', 1, ifname);
    return 0;
}


static int
iptablesRenameTmpRootChain(virConnectPtr conn ATTRIBUTE_UNUSED,
                           const char *iptables_cmd,
                           virBufferPtr buf,
                           char prefix,
                           int incoming,
                           const char *ifname)
{
    char tmpchain[MAX_CHAINNAME_LENGTH], chain[MAX_CHAINNAME_LENGTH];
    char tmpChainPrefix[2] = {
        prefix,
        (incoming) ? CHAINPREFIX_HOST_IN_TEMP
                   : CHAINPREFIX_HOST_OUT_TEMP
    };
    char chainPrefix[2] = {
        prefix,
        (incoming) ? CHAINPREFIX_HOST_IN
                   : CHAINPREFIX_HOST_OUT
    };

    PRINT_IPT_ROOT_CHAIN(tmpchain, tmpChainPrefix, ifname);
    PRINT_IPT_ROOT_CHAIN(   chain,    chainPrefix, ifname);

    virBufferVSprintf(buf,
                      "%s -E %s %s" CMD_SEPARATOR,
                      iptables_cmd,
                      tmpchain,
                      chain);
    return 0;
}


static int
iptablesRenameTmpRootChains(virConnectPtr conn,
                            const char *iptables_cmd,
                            virBufferPtr buf,
                            const char *ifname)
{
    iptablesRenameTmpRootChain(conn, iptables_cmd, buf, 'F', 0, ifname);
    iptablesRenameTmpRootChain(conn, iptables_cmd, buf, 'F', 1, ifname);
    iptablesRenameTmpRootChain(conn, iptables_cmd, buf, 'H', 1, ifname);
    return 0;
}


static void
iptablesInstCommand(virConnectPtr conn ATTRIBUTE_UNUSED,
                    virBufferPtr buf,
                    const char *templ, char cmd, int pos,
                    int stopOnError)
{
    char position[10] = { 0 };
    if (pos >= 0)
        snprintf(position, sizeof(position), "%d", pos);
    virBufferVSprintf(buf, templ, cmd, position);
    virBufferVSprintf(buf, CMD_SEPARATOR "%s",
                      CMD_STOPONERR(stopOnError));
}


static int
iptablesHandleSrcMacAddr(virConnectPtr conn ATTRIBUTE_UNUSED,
                         virBufferPtr buf,
                         virNWFilterHashTablePtr vars,
                         nwItemDescPtr srcMacAddr,
                         int directionIn ATTRIBUTE_UNUSED)
{
    char macaddr[VIR_MAC_STRING_BUFLEN];

    if (HAS_ENTRY_ITEM(srcMacAddr)) {
        if (printDataType(conn,
                          vars,
                          macaddr, sizeof(macaddr),
                          srcMacAddr))
            goto err_exit;

        virBufferVSprintf(buf,
                          " -m mac %s --mac-source %s",
                          ENTRY_GET_NEG_SIGN(srcMacAddr),
                          macaddr);
    }

    return 0;

err_exit:
    virBufferFreeAndReset(buf);

    return 1;
}


static int
iptablesHandleIpHdr(virConnectPtr conn ATTRIBUTE_UNUSED,
                    virBufferPtr buf,
                    virNWFilterHashTablePtr vars,
                    ipHdrDataDefPtr ipHdr,
                    int directionIn)
{
    char ipaddr[INET6_ADDRSTRLEN],
         number[20];
    const char *src = "--source";
    const char *dst = "--destination";
    const char *srcrange = "--src-range";
    const char *dstrange = "--dst-range";
    if (directionIn) {
        src = "--destination";
        dst = "--source";
        srcrange = "--dst-range";
        dstrange = "--src-range";
    }

    if (HAS_ENTRY_ITEM(&ipHdr->dataSrcIPAddr)) {

        if (printDataType(conn,
                          vars,
                          ipaddr, sizeof(ipaddr),
                          &ipHdr->dataSrcIPAddr))
            goto err_exit;

        virBufferVSprintf(buf,
                          " %s %s %s",
                          ENTRY_GET_NEG_SIGN(&ipHdr->dataSrcIPAddr),
                          src,
                          ipaddr);

        if (HAS_ENTRY_ITEM(&ipHdr->dataSrcIPMask)) {

            if (printDataType(conn,
                              vars,
                              number, sizeof(number),
                              &ipHdr->dataSrcIPMask))
                goto err_exit;

            virBufferVSprintf(buf,
                              "/%s",
                              number);
        }
    } else if (HAS_ENTRY_ITEM(&ipHdr->dataSrcIPFrom)) {

        if (printDataType(conn,
                          vars,
                          ipaddr, sizeof(ipaddr),
                          &ipHdr->dataSrcIPFrom))
            goto err_exit;

        virBufferVSprintf(buf,
                          " -m iprange %s %s %s",
                          ENTRY_GET_NEG_SIGN(&ipHdr->dataSrcIPFrom),
                          srcrange,
                          ipaddr);

        if (HAS_ENTRY_ITEM(&ipHdr->dataSrcIPTo)) {

            if (printDataType(conn,
                              vars,
                              ipaddr, sizeof(ipaddr),
                              &ipHdr->dataSrcIPTo))
                goto err_exit;

            virBufferVSprintf(buf,
                              "-%s",
                              ipaddr);
        }
    }

    if (HAS_ENTRY_ITEM(&ipHdr->dataDstIPAddr)) {

        if (printDataType(conn,
                          vars,
                          ipaddr, sizeof(ipaddr),
                          &ipHdr->dataDstIPAddr))
           goto err_exit;

        virBufferVSprintf(buf,
                          " %s %s %s",
                          ENTRY_GET_NEG_SIGN(&ipHdr->dataDstIPAddr),
                          dst,
                          ipaddr);

        if (HAS_ENTRY_ITEM(&ipHdr->dataDstIPMask)) {

            if (printDataType(conn,
                              vars,
                              number, sizeof(number),
                              &ipHdr->dataDstIPMask))
                goto err_exit;

            virBufferVSprintf(buf,
                              "/%s",
                              number);

        }
    } else if (HAS_ENTRY_ITEM(&ipHdr->dataDstIPFrom)) {

        if (printDataType(conn,
                          vars,
                          ipaddr, sizeof(ipaddr),
                          &ipHdr->dataDstIPFrom))
            goto err_exit;

        virBufferVSprintf(buf,
                          " -m iprange %s %s %s",
                          ENTRY_GET_NEG_SIGN(&ipHdr->dataDstIPFrom),
                          dstrange,
                          ipaddr);

        if (HAS_ENTRY_ITEM(&ipHdr->dataDstIPTo)) {

            if (printDataType(conn,
                              vars,
                              ipaddr, sizeof(ipaddr),
                              &ipHdr->dataDstIPTo))
                goto err_exit;

            virBufferVSprintf(buf,
                              "-%s",
                              ipaddr);
        }
    }

    if (HAS_ENTRY_ITEM(&ipHdr->dataDSCP)) {

        if (printDataType(conn,
                          vars,
                          number, sizeof(number),
                          &ipHdr->dataDSCP))
           goto err_exit;

        virBufferVSprintf(buf,
                          " -m dscp %s --dscp %s",
                          ENTRY_GET_NEG_SIGN(&ipHdr->dataDSCP),
                          number);
    }

    return 0;

err_exit:
    virBufferFreeAndReset(buf);

    return 1;
}


static int
iptablesHandlePortData(virConnectPtr conn ATTRIBUTE_UNUSED,
                       virBufferPtr buf,
                       virNWFilterHashTablePtr vars,
                       portDataDefPtr portData,
                       int directionIn)
{
    char portstr[20];
    const char *sport = "--sport";
    const char *dport = "--dport";
    if (directionIn) {
        sport = "--dport";
        dport = "--sport";
    }

    if (HAS_ENTRY_ITEM(&portData->dataSrcPortStart)) {
        if (printDataType(conn,
                          vars,
                          portstr, sizeof(portstr),
                          &portData->dataSrcPortStart))
            goto err_exit;

        virBufferVSprintf(buf,
                          " %s %s %s",
                          ENTRY_GET_NEG_SIGN(&portData->dataSrcPortStart),
                          sport,
                          portstr);

        if (HAS_ENTRY_ITEM(&portData->dataSrcPortEnd)) {
            if (printDataType(conn,
                              vars,
                              portstr, sizeof(portstr),
                              &portData->dataSrcPortEnd))
                goto err_exit;

             virBufferVSprintf(buf,
                               ":%s",
                               portstr);
        }
    }

    if (HAS_ENTRY_ITEM(&portData->dataDstPortStart)) {
        if (printDataType(conn,
                          vars,
                          portstr, sizeof(portstr),
                          &portData->dataDstPortStart))
            goto err_exit;

        virBufferVSprintf(buf,
                          " %s %s %s",
                          ENTRY_GET_NEG_SIGN(&portData->dataDstPortStart),
                          dport,
                          portstr);

        if (HAS_ENTRY_ITEM(&portData->dataDstPortEnd)) {
            if (printDataType(conn,
                              vars,
                              portstr, sizeof(portstr),
                              &portData->dataDstPortEnd))
                goto err_exit;

             virBufferVSprintf(buf,
                               ":%s",
                               portstr);
        }
    }

    return 0;

err_exit:
    return 1;
}

/*
 * _iptablesCreateRuleInstance:
 * @conn : Pointer to a virConnect object
 * @chainPrefix : The prefix to put in front of the name of the chain
 * @nwfilter : The filter
 * @rule: The rule of the filter to convert
 * @ifname : The name of the interface to apply the rule to
 * @vars : A map containing the variables to resolve
 * @res : The data structure to store the result(s) into
 *
 * Convert a single rule into its representation for later instantiation
 *
 * Returns 0 in case of success with the result stored in the data structure
 * pointed to by res, != 0 otherwise with the error message stored in the
 * virConnect object.
 */
static int
_iptablesCreateRuleInstance(virConnectPtr conn,
                            int directionIn,
                            const char *chainPrefix,
                            virNWFilterDefPtr nwfilter,
                            virNWFilterRuleDefPtr rule,
                            const char *ifname,
                            virNWFilterHashTablePtr vars,
                            virNWFilterRuleInstPtr res,
                            const char *match,
                            const char *accept_target,
                            bool isIPv6)
{
    char chain[MAX_CHAINNAME_LENGTH];
    char number[20];
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    const char *target;
    const char *iptables_cmd = (isIPv6) ? IP6TABLES_CMD : IPTABLES_CMD;

    PRINT_IPT_ROOT_CHAIN(chain, chainPrefix, ifname);

    switch (rule->prtclType) {
    case VIR_NWFILTER_RULE_PROTOCOL_TCP:
    case VIR_NWFILTER_RULE_PROTOCOL_TCPoIPV6:
        virBufferVSprintf(&buf,
                          CMD_DEF_PRE "%s -%%c %s %%s",
                          iptables_cmd,
                          chain);

        virBufferAddLit(&buf, " -p tcp");

        if (iptablesHandleSrcMacAddr(conn,
                                     &buf,
                                     vars,
                                     &rule->p.tcpHdrFilter.dataSrcMACAddr,
                                     directionIn))
            goto err_exit;

        if (iptablesHandleIpHdr(conn,
                                &buf,
                                vars,
                                &rule->p.tcpHdrFilter.ipHdr,
                                directionIn))
            goto err_exit;

        if (iptablesHandlePortData(conn,
                                   &buf,
                                   vars,
                                   &rule->p.tcpHdrFilter.portData,
                                   directionIn))
            goto err_exit;

        if (HAS_ENTRY_ITEM(&rule->p.tcpHdrFilter.dataTCPOption)) {
            if (printDataType(conn,
                              vars,
                              number, sizeof(number),
                              &rule->p.tcpHdrFilter.dataTCPOption))
                goto err_exit;

            virBufferVSprintf(&buf,
                              " %s --tcp-option %s",
                              ENTRY_GET_NEG_SIGN(&rule->p.tcpHdrFilter.dataTCPOption),
                              number);
        }

    break;

    case VIR_NWFILTER_RULE_PROTOCOL_UDP:
    case VIR_NWFILTER_RULE_PROTOCOL_UDPoIPV6:
        virBufferVSprintf(&buf,
                          CMD_DEF_PRE "%s -%%c %s %%s",
                          iptables_cmd,
                          chain);

        virBufferAddLit(&buf, " -p udp");

        if (iptablesHandleSrcMacAddr(conn,
                                     &buf,
                                     vars,
                                     &rule->p.udpHdrFilter.dataSrcMACAddr,
                                     directionIn))
            goto err_exit;

        if (iptablesHandleIpHdr(conn,
                                &buf,
                                vars,
                                &rule->p.udpHdrFilter.ipHdr,
                                directionIn))
            goto err_exit;

        if (iptablesHandlePortData(conn,
                                   &buf,
                                   vars,
                                   &rule->p.udpHdrFilter.portData,
                                   directionIn))
            goto err_exit;
    break;

    case VIR_NWFILTER_RULE_PROTOCOL_UDPLITE:
    case VIR_NWFILTER_RULE_PROTOCOL_UDPLITEoIPV6:
        virBufferVSprintf(&buf,
                          CMD_DEF_PRE "%s -%%c %s %%s",
                          iptables_cmd,
                          chain);

        virBufferAddLit(&buf, " -p udplite");

        if (iptablesHandleSrcMacAddr(conn,
                                     &buf,
                                     vars,
                                     &rule->p.udpliteHdrFilter.dataSrcMACAddr,
                                     directionIn))
            goto err_exit;

        if (iptablesHandleIpHdr(conn,
                                &buf,
                                vars,
                                &rule->p.udpliteHdrFilter.ipHdr,
                                directionIn))
            goto err_exit;

    break;

    case VIR_NWFILTER_RULE_PROTOCOL_ESP:
    case VIR_NWFILTER_RULE_PROTOCOL_ESPoIPV6:
        virBufferVSprintf(&buf,
                          CMD_DEF_PRE "%s -%%c %s %%s",
                          iptables_cmd,
                          chain);

        virBufferAddLit(&buf, " -p esp");

        if (iptablesHandleSrcMacAddr(conn,
                                     &buf,
                                     vars,
                                     &rule->p.espHdrFilter.dataSrcMACAddr,
                                     directionIn))
            goto err_exit;

        if (iptablesHandleIpHdr(conn,
                                &buf,
                                vars,
                                &rule->p.espHdrFilter.ipHdr,
                                directionIn))
            goto err_exit;

    break;

    case VIR_NWFILTER_RULE_PROTOCOL_AH:
    case VIR_NWFILTER_RULE_PROTOCOL_AHoIPV6:
        virBufferVSprintf(&buf,
                          CMD_DEF_PRE "%s -%%c %s %%s",
                          iptables_cmd,
                          chain);

        virBufferAddLit(&buf, " -p ah");

        if (iptablesHandleSrcMacAddr(conn,
                                     &buf,
                                     vars,
                                     &rule->p.ahHdrFilter.dataSrcMACAddr,
                                     directionIn))
            goto err_exit;

        if (iptablesHandleIpHdr(conn,
                                &buf,
                                vars,
                                &rule->p.ahHdrFilter.ipHdr,
                                directionIn))
            goto err_exit;

    break;

    case VIR_NWFILTER_RULE_PROTOCOL_SCTP:
    case VIR_NWFILTER_RULE_PROTOCOL_SCTPoIPV6:
        virBufferVSprintf(&buf,
                          CMD_DEF_PRE "%s -%%c %s %%s",
                          iptables_cmd,
                          chain);

        virBufferAddLit(&buf, " -p sctp");

        if (iptablesHandleSrcMacAddr(conn,
                                     &buf,
                                     vars,
                                     &rule->p.sctpHdrFilter.dataSrcMACAddr,
                                     directionIn))
            goto err_exit;

        if (iptablesHandleIpHdr(conn,
                                &buf,
                                vars,
                                &rule->p.sctpHdrFilter.ipHdr,
                                directionIn))
            goto err_exit;

        if (iptablesHandlePortData(conn,
                                   &buf,
                                   vars,
                                   &rule->p.sctpHdrFilter.portData,
                                   directionIn))
            goto err_exit;
    break;

    case VIR_NWFILTER_RULE_PROTOCOL_ICMP:
    case VIR_NWFILTER_RULE_PROTOCOL_ICMPV6:
        virBufferVSprintf(&buf,
                          CMD_DEF_PRE "%s -%%c %s %%s",
                          iptables_cmd,
                          chain);

        if (rule->prtclType == VIR_NWFILTER_RULE_PROTOCOL_ICMP)
            virBufferAddLit(&buf, " -p icmp");
        else
            virBufferAddLit(&buf, " -p icmpv6");

        if (iptablesHandleSrcMacAddr(conn,
                                     &buf,
                                     vars,
                                     &rule->p.icmpHdrFilter.dataSrcMACAddr,
                                     directionIn))
            goto err_exit;

        if (iptablesHandleIpHdr(conn,
                                &buf,
                                vars,
                                &rule->p.icmpHdrFilter.ipHdr,
                                directionIn))
            goto err_exit;

        if (HAS_ENTRY_ITEM(&rule->p.icmpHdrFilter.dataICMPType)) {
            if (printDataType(conn,
                              vars,
                              number, sizeof(number),
                              &rule->p.icmpHdrFilter.dataICMPType))
                goto err_exit;

            virBufferVSprintf(&buf,
                      " %s --icmp-type %s",
                      ENTRY_GET_NEG_SIGN(&rule->p.icmpHdrFilter.dataICMPType),
                      number);

            if (HAS_ENTRY_ITEM(&rule->p.icmpHdrFilter.dataICMPCode)) {
                if (printDataType(conn,
                                  vars,
                                  number, sizeof(number),
                                  &rule->p.icmpHdrFilter.dataICMPCode))
                    goto err_exit;

                 virBufferVSprintf(&buf,
                                   "/%s",
                                   number);
            }
        }
    break;

    case VIR_NWFILTER_RULE_PROTOCOL_ALL:
    case VIR_NWFILTER_RULE_PROTOCOL_ALLoIPV6:
        virBufferVSprintf(&buf,
                          CMD_DEF_PRE "%s -%%c %s %%s",
                          iptables_cmd,
                          chain);

        virBufferAddLit(&buf, " -p all");

        if (iptablesHandleSrcMacAddr(conn,
                                     &buf,
                                     vars,
                                     &rule->p.allHdrFilter.dataSrcMACAddr,
                                     directionIn))
            goto err_exit;

        if (iptablesHandleIpHdr(conn,
                                &buf,
                                vars,
                                &rule->p.allHdrFilter.ipHdr,
                                directionIn))
            goto err_exit;

    break;

    default:
        return -1;
    }

    if (match)
        virBufferVSprintf(&buf, " %s", match);

    if (rule->action == VIR_NWFILTER_RULE_ACTION_ACCEPT)
        target = accept_target;
    else
        target = "DROP";

    virBufferVSprintf(&buf,
                      " -j %s" CMD_DEF_POST CMD_SEPARATOR
                      CMD_EXEC,
                      target);

    if (virBufferError(&buf)) {
        virBufferFreeAndReset(&buf);
        virReportOOMError();
        return -1;
    }

    return ebiptablesAddRuleInst(conn,
                                 res,
                                 virBufferContentAndReset(&buf),
                                 nwfilter->chainsuffix,
                                 '\0',
                                 rule->priority,
                                 (isIPv6) ? RT_IP6TABLES : RT_IPTABLES);


err_exit:
    virBufferFreeAndReset(&buf);

    return -1;

}


static int
iptablesCreateRuleInstance(virConnectPtr conn,
                           virNWFilterDefPtr nwfilter,
                           virNWFilterRuleDefPtr rule,
                           const char *ifname,
                           virNWFilterHashTablePtr vars,
                           virNWFilterRuleInstPtr res,
                           bool isIPv6)
{
    int rc;
    int directionIn = 0;
    char chainPrefix[2];
    int needState = 1;

    if ((rule->tt == VIR_NWFILTER_RULE_DIRECTION_IN) ||
        (rule->tt == VIR_NWFILTER_RULE_DIRECTION_INOUT)) {
        directionIn = 1;
        needState = 0;
    }

    chainPrefix[0] = 'F';

    chainPrefix[1] = CHAINPREFIX_HOST_IN_TEMP;
    rc = _iptablesCreateRuleInstance(conn,
                                     directionIn,
                                     chainPrefix,
                                     nwfilter,
                                     rule,
                                     ifname,
                                     vars,
                                     res,
                                     needState ? MATCH_STATE_OUT
                                               : NULL,
                                     "RETURN",
                                     isIPv6);
    if (rc)
        return rc;

    chainPrefix[1] = CHAINPREFIX_HOST_OUT_TEMP;
    rc = _iptablesCreateRuleInstance(conn,
                                     !directionIn,
                                     chainPrefix,
                                     nwfilter,
                                     rule,
                                     ifname,
                                     vars,
                                     res,
                                     needState ? MATCH_STATE_IN
                                               : NULL,
                                     "ACCEPT",
                                     isIPv6);
    if (rc)
        return rc;

    chainPrefix[0] = 'H';
    chainPrefix[1] = CHAINPREFIX_HOST_IN_TEMP;
    rc = _iptablesCreateRuleInstance(conn,
                                     directionIn,
                                     chainPrefix,
                                     nwfilter,
                                     rule,
                                     ifname,
                                     vars,
                                     res,
                                     NULL,
                                     "ACCEPT",
                                     isIPv6);
    if (rc)
        return rc;

    return rc;
}




/*
 * ebtablesCreateRuleInstance:
 * @conn : Pointer to a virConnect object
 * @chainPrefix : The prefix to put in front of the name of the chain
 * @nwfilter : The filter
 * @rule: The rule of the filter to convert
 * @ifname : The name of the interface to apply the rule to
 * @vars : A map containing the variables to resolve
 * @res : The data structure to store the result(s) into
 *
 * Convert a single rule into its representation for later instantiation
 *
 * Returns 0 in case of success with the result stored in the data structure
 * pointed to by res, != 0 otherwise with the error message stored in the
 * virConnect object.
 */
static int
ebtablesCreateRuleInstance(virConnectPtr conn,
                           char chainPrefix,
                           virNWFilterDefPtr nwfilter,
                           virNWFilterRuleDefPtr rule,
                           const char *ifname,
                           virNWFilterHashTablePtr vars,
                           virNWFilterRuleInstPtr res)
{
    char macaddr[VIR_MAC_STRING_BUFLEN],
         ipaddr[INET_ADDRSTRLEN],
         ipv6addr[INET6_ADDRSTRLEN],
         number[20];
    char chain[MAX_CHAINNAME_LENGTH];
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (nwfilter->chainsuffix == VIR_NWFILTER_CHAINSUFFIX_ROOT)
        PRINT_ROOT_CHAIN(chain, chainPrefix, ifname);
    else
        PRINT_CHAIN(chain, chainPrefix, ifname,
                    virNWFilterChainSuffixTypeToString(nwfilter->chainsuffix));


    switch (rule->prtclType) {
    case VIR_NWFILTER_RULE_PROTOCOL_MAC:

        virBufferVSprintf(&buf,
                          CMD_DEF_PRE EBTABLES_CMD " -t %s -%%c %s %%s",
                          EBTABLES_DEFAULT_TABLE, chain);


        if (ebtablesHandleEthHdr(conn,
                                 &buf,
                                 vars,
                                 &rule->p.ethHdrFilter.ethHdr))
            goto err_exit;

        if (HAS_ENTRY_ITEM(&rule->p.ethHdrFilter.dataProtocolID)) {
            if (printDataType(conn,
                              vars,
                              number, sizeof(number),
                              &rule->p.ethHdrFilter.dataProtocolID))
                goto err_exit;
            virBufferVSprintf(&buf,
                          " -p %s %s",
                          ENTRY_GET_NEG_SIGN(&rule->p.ethHdrFilter.dataProtocolID),
                          number);
        }
    break;

    case VIR_NWFILTER_RULE_PROTOCOL_ARP:

        virBufferVSprintf(&buf,
                          CMD_DEF_PRE EBTABLES_CMD " -t %s -%%c %s %%s",
                          EBTABLES_DEFAULT_TABLE, chain);

        if (ebtablesHandleEthHdr(conn,
                                 &buf,
                                 vars,
                                 &rule->p.arpHdrFilter.ethHdr))
            goto err_exit;

        virBufferAddLit(&buf, " -p arp");

        if (HAS_ENTRY_ITEM(&rule->p.arpHdrFilter.dataHWType)) {
             if (printDataType(conn,
                              vars,
                              number, sizeof(number),
                              &rule->p.arpHdrFilter.dataHWType))
                goto err_exit;
           virBufferVSprintf(&buf,
                          " --arp-htype %s %s",
                          ENTRY_GET_NEG_SIGN(&rule->p.arpHdrFilter.dataHWType),
                          number);
        }

        if (HAS_ENTRY_ITEM(&rule->p.arpHdrFilter.dataOpcode)) {
            if (printDataType(conn,
                              vars,
                              number, sizeof(number),
                              &rule->p.arpHdrFilter.dataOpcode))
                goto err_exit;
            virBufferVSprintf(&buf,
                          " --arp-opcode %s %s",
                          ENTRY_GET_NEG_SIGN(&rule->p.arpHdrFilter.dataOpcode),
                          number);
        }

        if (HAS_ENTRY_ITEM(&rule->p.arpHdrFilter.dataProtocolType)) {
            if (printDataType(conn,
                              vars,
                              number, sizeof(number),
                              &rule->p.arpHdrFilter.dataProtocolType))
                goto err_exit;
            virBufferVSprintf(&buf,
                          " --arp-ptype %s %s",
                          ENTRY_GET_NEG_SIGN(&rule->p.arpHdrFilter.dataProtocolType),
                          number);
        }

        if (HAS_ENTRY_ITEM(&rule->p.arpHdrFilter.dataARPSrcIPAddr)) {
            if (printDataType(conn,
                              vars,
                              ipaddr, sizeof(ipaddr),
                              &rule->p.arpHdrFilter.dataARPSrcIPAddr))
                goto err_exit;

            virBufferVSprintf(&buf,
                          " --arp-ip-src %s %s",
                          ENTRY_GET_NEG_SIGN(&rule->p.arpHdrFilter.dataARPSrcIPAddr),
                          ipaddr);
        }

        if (HAS_ENTRY_ITEM(&rule->p.arpHdrFilter.dataARPDstIPAddr)) {
            if (printDataType(conn,
                              vars,
                              ipaddr, sizeof(ipaddr),
                              &rule->p.arpHdrFilter.dataARPDstIPAddr))
                goto err_exit;

            virBufferVSprintf(&buf,
                          " --arp-ip-dst %s %s",
                          ENTRY_GET_NEG_SIGN(&rule->p.arpHdrFilter.dataARPDstIPAddr),
                          ipaddr);
        }

        if (HAS_ENTRY_ITEM(&rule->p.arpHdrFilter.dataARPSrcMACAddr)) {
            if (printDataType(conn,
                              vars,
                              macaddr, sizeof(macaddr),
                              &rule->p.arpHdrFilter.dataARPSrcMACAddr))
                goto err_exit;

            virBufferVSprintf(&buf,
                          " --arp-mac-src %s %s",
                          ENTRY_GET_NEG_SIGN(&rule->p.arpHdrFilter.dataARPSrcMACAddr),
                          macaddr);
        }

        if (HAS_ENTRY_ITEM(&rule->p.arpHdrFilter.dataARPDstMACAddr)) {
            if (printDataType(conn,
                              vars,
                              macaddr, sizeof(macaddr),
                              &rule->p.arpHdrFilter.dataARPDstMACAddr))
                goto err_exit;

            virBufferVSprintf(&buf,
                          " --arp-mac-dst %s %s",
                          ENTRY_GET_NEG_SIGN(&rule->p.arpHdrFilter.dataARPDstMACAddr),
                          macaddr);
        }
    break;

    case VIR_NWFILTER_RULE_PROTOCOL_IP:
        virBufferVSprintf(&buf,
                          CMD_DEF_PRE EBTABLES_CMD " -t %s -%%c %s %%s",
                          EBTABLES_DEFAULT_TABLE, chain);

        if (ebtablesHandleEthHdr(conn,
                                 &buf,
                                 vars,
                                 &rule->p.ipHdrFilter.ethHdr))
            goto err_exit;

        virBufferAddLit(&buf,
                        " -p ipv4");

        if (HAS_ENTRY_ITEM(&rule->p.ipHdrFilter.ipHdr.dataSrcIPAddr)) {
            if (printDataType(conn,
                              vars,
                              ipaddr, sizeof(ipaddr),
                              &rule->p.ipHdrFilter.ipHdr.dataSrcIPAddr))
                goto err_exit;

            virBufferVSprintf(&buf,
                          " --ip-source %s %s",
                          ENTRY_GET_NEG_SIGN(&rule->p.ipHdrFilter.ipHdr.dataSrcIPAddr),
                          ipaddr);

            if (HAS_ENTRY_ITEM(&rule->p.ipHdrFilter.ipHdr.dataSrcIPMask)) {
                if (printDataType(conn,
                                  vars,
                                  number, sizeof(number),
                                  &rule->p.ipHdrFilter.ipHdr.dataSrcIPMask))
                    goto err_exit;
                virBufferVSprintf(&buf,
                             "/%s",
                             number);
            }
        }

        if (HAS_ENTRY_ITEM(&rule->p.ipHdrFilter.ipHdr.dataDstIPAddr)) {

            if (printDataType(conn,
                              vars,
                              ipaddr, sizeof(ipaddr),
                              &rule->p.ipHdrFilter.ipHdr.dataDstIPAddr))
                goto err_exit;

            virBufferVSprintf(&buf,
                          " --ip-destination %s %s",
                          ENTRY_GET_NEG_SIGN(&rule->p.ipHdrFilter.ipHdr.dataDstIPAddr),
                          ipaddr);

            if (HAS_ENTRY_ITEM(&rule->p.ipHdrFilter.ipHdr.dataDstIPMask)) {
                if (printDataType(conn,
                                  vars,
                                  number, sizeof(number),
                                  &rule->p.ipHdrFilter.ipHdr.dataDstIPMask))
                    goto err_exit;
                virBufferVSprintf(&buf,
                                  "/%s",
                                  number);
            }
        }

        if (HAS_ENTRY_ITEM(&rule->p.ipHdrFilter.ipHdr.dataProtocolID)) {
            if (printDataType(conn,
                              vars,
                              number, sizeof(number),
                              &rule->p.ipHdrFilter.ipHdr.dataProtocolID))
                goto err_exit;

            virBufferVSprintf(&buf,
                 " --ip-protocol %s %s",
                 ENTRY_GET_NEG_SIGN(&rule->p.ipHdrFilter.ipHdr.dataProtocolID),
                 number);
        }

        if (HAS_ENTRY_ITEM(&rule->p.ipHdrFilter.portData.dataSrcPortStart)) {

            if (printDataType(conn,
                              vars,
                              number, sizeof(number),
                              &rule->p.ipHdrFilter.portData.dataSrcPortStart))
                goto err_exit;

            virBufferVSprintf(&buf,
                          " --ip-source-port %s %s",
                          ENTRY_GET_NEG_SIGN(&rule->p.ipHdrFilter.portData.dataSrcPortStart),
                          number);

            if (HAS_ENTRY_ITEM(&rule->p.ipHdrFilter.portData.dataSrcPortEnd)) {
                if (printDataType(conn,
                                  vars,
                                  number, sizeof(number),
                                  &rule->p.ipHdrFilter.portData.dataSrcPortEnd))
                    goto err_exit;

                virBufferVSprintf(&buf,
                                  ":%s",
                                  number);
            }
        }

        if (HAS_ENTRY_ITEM(&rule->p.ipHdrFilter.portData.dataDstPortStart)) {

            if (printDataType(conn,
                              vars,
                              number, sizeof(number),
                              &rule->p.ipHdrFilter.portData.dataDstPortStart))
                goto err_exit;

            virBufferVSprintf(&buf,
                          " --ip-destination-port %s %s",
                          ENTRY_GET_NEG_SIGN(&rule->p.ipHdrFilter.portData.dataDstPortStart),
                          number);

            if (HAS_ENTRY_ITEM(&rule->p.ipHdrFilter.portData.dataDstPortEnd)) {
                if (printDataType(conn,
                                vars,
                                number, sizeof(number),
                                &rule->p.ipHdrFilter.portData.dataDstPortEnd))
                    goto err_exit;

                virBufferVSprintf(&buf,
                                  ":%s",
                                  number);
            }
        }

        if (HAS_ENTRY_ITEM(&rule->p.ipHdrFilter.ipHdr.dataDSCP)) {
            if (printDataType(conn,
                              vars,
                              number, sizeof(number),
                              &rule->p.ipHdrFilter.ipHdr.dataDSCP))
                goto err_exit;

            virBufferVSprintf(&buf,
                       " --ip-tos %s %s",
                       ENTRY_GET_NEG_SIGN(&rule->p.ipHdrFilter.ipHdr.dataDSCP),
                       number);
        }
    break;

    case VIR_NWFILTER_RULE_PROTOCOL_IPV6:
        virBufferVSprintf(&buf,
                          CMD_DEF_PRE EBTABLES_CMD " -t %s -%%c %s %%s",
                          EBTABLES_DEFAULT_TABLE, chain);

        if (ebtablesHandleEthHdr(conn,
                                 &buf,
                                 vars,
                                 &rule->p.ipv6HdrFilter.ethHdr))
            goto err_exit;

        virBufferAddLit(&buf,
                        " -p ipv6");

        if (HAS_ENTRY_ITEM(&rule->p.ipv6HdrFilter.ipHdr.dataSrcIPAddr)) {
            if (printDataType(conn,
                              vars,
                              ipv6addr, sizeof(ipv6addr),
                              &rule->p.ipv6HdrFilter.ipHdr.dataSrcIPAddr))
                goto err_exit;

            virBufferVSprintf(&buf,
                          " --ip6-source %s %s",
                          ENTRY_GET_NEG_SIGN(&rule->p.ipv6HdrFilter.ipHdr.dataSrcIPAddr),
                          ipv6addr);

            if (HAS_ENTRY_ITEM(&rule->p.ipv6HdrFilter.ipHdr.dataSrcIPMask)) {
                if (printDataType(conn,
                                  vars,
                                  number, sizeof(number),
                                  &rule->p.ipv6HdrFilter.ipHdr.dataSrcIPMask))
                    goto err_exit;
                virBufferVSprintf(&buf,
                             "/%s",
                             number);
            }
        }

        if (HAS_ENTRY_ITEM(&rule->p.ipv6HdrFilter.ipHdr.dataDstIPAddr)) {

            if (printDataType(conn,
                              vars,
                              ipv6addr, sizeof(ipv6addr),
                              &rule->p.ipv6HdrFilter.ipHdr.dataDstIPAddr))
                goto err_exit;

            virBufferVSprintf(&buf,
                          " --ip6-destination %s %s",
                          ENTRY_GET_NEG_SIGN(&rule->p.ipv6HdrFilter.ipHdr.dataDstIPAddr),
                          ipv6addr);

            if (HAS_ENTRY_ITEM(&rule->p.ipv6HdrFilter.ipHdr.dataDstIPMask)) {
                if (printDataType(conn,
                                  vars,
                                  number, sizeof(number),
                                  &rule->p.ipv6HdrFilter.ipHdr.dataDstIPMask))
                    goto err_exit;
                virBufferVSprintf(&buf,
                                  "/%s",
                                  number);
            }
        }

        if (HAS_ENTRY_ITEM(&rule->p.ipv6HdrFilter.ipHdr.dataProtocolID)) {
            if (printDataType(conn,
                              vars,
                              number, sizeof(number),
                              &rule->p.ipv6HdrFilter.ipHdr.dataProtocolID))
                goto err_exit;

            virBufferVSprintf(&buf,
                 " --ip6-protocol %s %s",
                 ENTRY_GET_NEG_SIGN(&rule->p.ipv6HdrFilter.ipHdr.dataProtocolID),
                 number);
        }

        if (HAS_ENTRY_ITEM(&rule->p.ipv6HdrFilter.portData.dataSrcPortStart)) {

            if (printDataType(conn,
                              vars,
                              number, sizeof(number),
                              &rule->p.ipv6HdrFilter.portData.dataSrcPortStart))
                goto err_exit;

            virBufferVSprintf(&buf,
                          " --ip6-source-port %s %s",
                          ENTRY_GET_NEG_SIGN(&rule->p.ipv6HdrFilter.portData.dataSrcPortStart),
                          number);

            if (HAS_ENTRY_ITEM(&rule->p.ipv6HdrFilter.portData.dataSrcPortEnd)) {
                if (printDataType(conn,
                                  vars,
                                  number, sizeof(number),
                                  &rule->p.ipv6HdrFilter.portData.dataSrcPortEnd))
                    goto err_exit;

                virBufferVSprintf(&buf,
                                  ":%s",
                                  number);
            }
        }

        if (HAS_ENTRY_ITEM(&rule->p.ipv6HdrFilter.portData.dataDstPortStart)) {

            if (printDataType(conn,
                              vars,
                              number, sizeof(number),
                              &rule->p.ipv6HdrFilter.portData.dataDstPortStart))
                goto err_exit;

            virBufferVSprintf(&buf,
                          " --ip6-destination-port %s %s",
                          ENTRY_GET_NEG_SIGN(&rule->p.ipv6HdrFilter.portData.dataDstPortStart),
                          number);

            if (HAS_ENTRY_ITEM(&rule->p.ipv6HdrFilter.portData.dataDstPortEnd)) {
                if (printDataType(conn,
                                vars,
                                number, sizeof(number),
                                &rule->p.ipv6HdrFilter.portData.dataDstPortEnd))
                    goto err_exit;

                virBufferVSprintf(&buf,
                                  ":%s",
                                  number);
            }
        }
    break;

    case VIR_NWFILTER_RULE_PROTOCOL_NONE:
        virBufferVSprintf(&buf,
                          CMD_DEF_PRE EBTABLES_CMD " -t %s -%%c %s %%s",
                          EBTABLES_DEFAULT_TABLE, chain);
    break;

    default:
        return -1;
    }

    virBufferVSprintf(&buf,
                      " -j %s" CMD_DEF_POST CMD_SEPARATOR
                      CMD_EXEC,
                      virNWFilterJumpTargetTypeToString(rule->action));

    if (virBufferError(&buf)) {
        virBufferFreeAndReset(&buf);
        virReportOOMError();
        return -1;
    }

    return ebiptablesAddRuleInst(conn,
                                 res,
                                 virBufferContentAndReset(&buf),
                                 nwfilter->chainsuffix,
                                 chainPrefix,
                                 rule->priority,
                                 RT_EBTABLES);

err_exit:
    virBufferFreeAndReset(&buf);

    return -1;
}


/*
 * ebiptablesCreateRuleInstance:
 * @conn : Pointer to a virConnect object
 * @nwfilter : The filter
 * @rule: The rule of the filter to convert
 * @ifname : The name of the interface to apply the rule to
 * @vars : A map containing the variables to resolve
 * @res : The data structure to store the result(s) into
 *
 * Convert a single rule into its representation for later instantiation
 *
 * Returns 0 in case of success with the result stored in the data structure
 * pointed to by res, != 0 otherwise with the error message stored in the
 * virConnect object.
 */
static int
ebiptablesCreateRuleInstance(virConnectPtr conn,
                             enum virDomainNetType nettype,
                             virNWFilterDefPtr nwfilter,
                             virNWFilterRuleDefPtr rule,
                             const char *ifname,
                             virNWFilterHashTablePtr vars,
                             virNWFilterRuleInstPtr res)
{
    int rc = 0;
    bool isIPv6;

    switch (rule->prtclType) {
    case VIR_NWFILTER_RULE_PROTOCOL_IP:
    case VIR_NWFILTER_RULE_PROTOCOL_MAC:
    case VIR_NWFILTER_RULE_PROTOCOL_ARP:
    case VIR_NWFILTER_RULE_PROTOCOL_NONE:
    case VIR_NWFILTER_RULE_PROTOCOL_IPV6:

        if (rule->tt == VIR_NWFILTER_RULE_DIRECTION_OUT ||
            rule->tt == VIR_NWFILTER_RULE_DIRECTION_INOUT) {
            rc = ebtablesCreateRuleInstance(conn,
                                            CHAINPREFIX_HOST_IN_TEMP,
                                            nwfilter,
                                            rule,
                                            ifname,
                                            vars,
                                            res);
            if (rc)
                return rc;
        }

        if (rule->tt == VIR_NWFILTER_RULE_DIRECTION_IN ||
            rule->tt == VIR_NWFILTER_RULE_DIRECTION_INOUT) {
            rc = ebtablesCreateRuleInstance(conn,
                                            CHAINPREFIX_HOST_OUT_TEMP,
                                            nwfilter,
                                            rule,
                                            ifname,
                                            vars,
                                            res);
        }
    break;

    case VIR_NWFILTER_RULE_PROTOCOL_TCP:
    case VIR_NWFILTER_RULE_PROTOCOL_UDP:
    case VIR_NWFILTER_RULE_PROTOCOL_UDPLITE:
    case VIR_NWFILTER_RULE_PROTOCOL_ESP:
    case VIR_NWFILTER_RULE_PROTOCOL_AH:
    case VIR_NWFILTER_RULE_PROTOCOL_SCTP:
    case VIR_NWFILTER_RULE_PROTOCOL_ICMP:
    case VIR_NWFILTER_RULE_PROTOCOL_IGMP:
    case VIR_NWFILTER_RULE_PROTOCOL_ALL:
        if (nettype == VIR_DOMAIN_NET_TYPE_DIRECT) {
            virNWFilterReportError(conn, VIR_ERR_INVALID_NWFILTER,
                          _("'%s' protocol not support for net type '%s'"),
                          virNWFilterRuleProtocolTypeToString(rule->prtclType),
                          virDomainNetTypeToString(nettype));
            return 1;
        }
        isIPv6 = 0;
        rc = iptablesCreateRuleInstance(conn,
                                        nwfilter,
                                        rule,
                                        ifname,
                                        vars,
                                        res,
                                        isIPv6);
    break;

    case VIR_NWFILTER_RULE_PROTOCOL_TCPoIPV6:
    case VIR_NWFILTER_RULE_PROTOCOL_UDPoIPV6:
    case VIR_NWFILTER_RULE_PROTOCOL_UDPLITEoIPV6:
    case VIR_NWFILTER_RULE_PROTOCOL_ESPoIPV6:
    case VIR_NWFILTER_RULE_PROTOCOL_AHoIPV6:
    case VIR_NWFILTER_RULE_PROTOCOL_SCTPoIPV6:
    case VIR_NWFILTER_RULE_PROTOCOL_ICMPV6:
    case VIR_NWFILTER_RULE_PROTOCOL_ALLoIPV6:
        if (nettype == VIR_DOMAIN_NET_TYPE_DIRECT) {
            virNWFilterReportError(conn, VIR_ERR_INVALID_NWFILTER,
                          _("'%s' protocol not support for net type '%s'"),
                          virNWFilterRuleProtocolTypeToString(rule->prtclType),
                          virDomainNetTypeToString(nettype));
            return 1;
        }
        isIPv6 = 1;
        rc = iptablesCreateRuleInstance(conn,
                                        nwfilter,
                                        rule,
                                        ifname,
                                        vars,
                                        res,
                                        isIPv6);
    break;

    case VIR_NWFILTER_RULE_PROTOCOL_LAST:
        virNWFilterReportError(conn, VIR_ERR_INVALID_NWFILTER,
                               "%s", _("illegal protocol type"));
        rc = 1;
    break;
    }

    return rc;
}


static int
ebiptablesFreeRuleInstance(void *_inst)
{
    ebiptablesRuleInstFree((ebiptablesRuleInstPtr)_inst);
    return 0;
}


static int
ebiptablesDisplayRuleInstance(virConnectPtr conn ATTRIBUTE_UNUSED,
                              void *_inst)
{
    ebiptablesRuleInstPtr inst = (ebiptablesRuleInstPtr)_inst;
    printf("Command Template: %s\nNeeded protocol: %s\n\n",
           inst->commandTemplate,
           virNWFilterChainSuffixTypeToString(inst->neededProtocolChain));
    return 0;
}


/**
 * ebiptablesWriteToTempFile:
 * @conn: pointer to virConnect object
 * @string : the string to write into the file
 *
 * Returns the tempory filename where the string was written into,
 * NULL in case of error with the error reported.
 *
 * Write the string into a temporary file and return the name of
 * the temporary file. The string is assumed to contain executable
 * commands. A line '#!/bin/bash' will automatically be written
 * as the first line in the file. The permissions of the file are
 * set so that the file can be run as an executable script.
 */
static char *
ebiptablesWriteToTempFile(virConnectPtr conn,
                          const char *string) {
    char filename[] = "/tmp/virtdXXXXXX";
    int len;
    char *filnam;
    const char header[] = "#!" BASH_CMD "\n";
    size_t written;

    int fd = mkstemp(filename);

    if (fd < 0) {
        virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                               "%s",
                               _("cannot create temporary file"));
        return NULL;
    }

    if (fchmod(fd, S_IXUSR| S_IRUSR | S_IWUSR) < 0) {
        virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                               "%s",
                               _("cannot change permissions on temp. file"));
        goto err_exit;
    }

    len = strlen(header);
    written = safewrite(fd, header, len);
    if (written != len) {
        virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                               "%s",
                               _("cannot write string to file"));
        goto err_exit;
    }

    len = strlen(string);
    written = safewrite(fd, string, len);
    if (written != len) {
        virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                               "%s",
                               _("cannot write string to file"));
        goto err_exit;
    }

    filnam = strdup(filename);
    if (!filnam) {
        virReportOOMError();
        goto err_exit;
    }

    close(fd);
    return filnam;

err_exit:
    close(fd);
    unlink(filename);
    return NULL;
}


/**
 * ebiptablesExecCLI:
 * @conn : pointer to virConnect object
 * @buf : pointer to virBuffer containing the string with the commands to
 *        execute.
 * @status: Pointer to an integer for returning the status of the
 *        commands executed via the script the was run.
 *
 * Returns 0 in case of success, != 0 in case of an error. The returned
 * value is NOT the result of running the commands inside the bash
 * script.
 *
 * Execute a sequence of commands (held in the given buffer) as a bash
 * script and return the status of the execution.
 */
static int
ebiptablesExecCLI(virConnectPtr conn,
                  virBufferPtr buf,
                  int *status)
{
    char *cmds;
    char *filename;
    int rc;
    const char *argv[] = {NULL, NULL};

    if (virBufferError(buf)) {
        virReportOOMError();
        virBufferFreeAndReset(buf);
        return 1;
    }

    *status = 0;

    cmds = virBufferContentAndReset(buf);

    VIR_DEBUG("%s", cmds);

    if (!cmds)
        return 0;

    filename = ebiptablesWriteToTempFile(conn, cmds);
    VIR_FREE(cmds);

    if (!filename)
        return 1;

    argv[0] = filename;
    rc = virRun(argv, status);

    *status >>= 8;

    VIR_DEBUG("rc = %d, status = %d\n",rc, *status);

    unlink(filename);

    VIR_FREE(filename);

    return rc;
}


static int
ebtablesCreateTmpRootChain(virConnectPtr conn ATTRIBUTE_UNUSED,
                           virBufferPtr buf,
                           int incoming, const char *ifname,
                           int stopOnError)
{
    char chain[MAX_CHAINNAME_LENGTH];
    char chainPrefix = (incoming) ? CHAINPREFIX_HOST_IN_TEMP
                                  : CHAINPREFIX_HOST_OUT_TEMP;

    PRINT_ROOT_CHAIN(chain, chainPrefix, ifname);

    virBufferVSprintf(buf,
                      CMD_DEF(EBTABLES_CMD " -t %s -N %s") CMD_SEPARATOR
                      CMD_EXEC
                      "%s",
                      EBTABLES_DEFAULT_TABLE, chain,
                      CMD_STOPONERR(stopOnError));

    return 0;
}


static int
ebtablesLinkTmpRootChain(virConnectPtr conn ATTRIBUTE_UNUSED,
                         virBufferPtr buf,
                         int incoming, const char *ifname,
                         int stopOnError)
{
    char chain[MAX_CHAINNAME_LENGTH];
    char chainPrefix = (incoming) ? CHAINPREFIX_HOST_IN_TEMP
                                  : CHAINPREFIX_HOST_OUT_TEMP;
    char iodev = (incoming) ? 'i' : 'o';

    PRINT_ROOT_CHAIN(chain, chainPrefix, ifname);

    virBufferVSprintf(buf,
                      CMD_DEF(EBTABLES_CMD " -t %s -A %s -%c %s -j %s") CMD_SEPARATOR
                      CMD_EXEC
                      "%s",
                      EBTABLES_DEFAULT_TABLE,
                      (incoming) ? EBTABLES_CHAIN_INCOMING
                                 : EBTABLES_CHAIN_OUTGOING,
                      iodev, ifname, chain,

                      CMD_STOPONERR(stopOnError));

    return 0;
}


static int
_ebtablesRemoveRootChain(virConnectPtr conn ATTRIBUTE_UNUSED,
                         virBufferPtr buf,
                         int incoming, const char *ifname,
                         int isTempChain)
{
    char chain[MAX_CHAINNAME_LENGTH];
    char chainPrefix;
    if (isTempChain)
        chainPrefix = (incoming) ? CHAINPREFIX_HOST_IN_TEMP
                                 : CHAINPREFIX_HOST_OUT_TEMP;
    else
        chainPrefix = (incoming) ? CHAINPREFIX_HOST_IN
                                 : CHAINPREFIX_HOST_OUT;

    PRINT_ROOT_CHAIN(chain, chainPrefix, ifname);

    virBufferVSprintf(buf,
                      EBTABLES_CMD " -t %s -F %s" CMD_SEPARATOR
                      EBTABLES_CMD " -t %s -X %s" CMD_SEPARATOR,
                      EBTABLES_DEFAULT_TABLE, chain,
                      EBTABLES_DEFAULT_TABLE, chain);

    return 0;
}


static int
ebtablesRemoveRootChain(virConnectPtr conn,
                        virBufferPtr buf,
                        int incoming, const char *ifname)
{
    return _ebtablesRemoveRootChain(conn, buf, incoming, ifname, 0);
}


static int
ebtablesRemoveTmpRootChain(virConnectPtr conn,
                           virBufferPtr buf,
                           int incoming, const char *ifname)
{
    return _ebtablesRemoveRootChain(conn, buf, incoming, ifname, 1);
}


static int
_ebtablesUnlinkRootChain(virConnectPtr conn ATTRIBUTE_UNUSED,
                         virBufferPtr buf,
                         int incoming, const char *ifname,
                         int isTempChain)
{
    char chain[MAX_CHAINNAME_LENGTH];
    char iodev = (incoming) ? 'i' : 'o';
    char chainPrefix;

    if (isTempChain) {
        chainPrefix = (incoming) ? CHAINPREFIX_HOST_IN_TEMP
                                 : CHAINPREFIX_HOST_OUT_TEMP;
    } else {
        chainPrefix = (incoming) ? CHAINPREFIX_HOST_IN
                                 : CHAINPREFIX_HOST_OUT;
    }

    PRINT_ROOT_CHAIN(chain, chainPrefix, ifname);

    virBufferVSprintf(buf,
                      EBTABLES_CMD " -t %s -D %s -%c %s -j %s" CMD_SEPARATOR,
                      EBTABLES_DEFAULT_TABLE,
                      (incoming) ? EBTABLES_CHAIN_INCOMING
                                 : EBTABLES_CHAIN_OUTGOING,
                      iodev, ifname, chain);

    return 0;
}


static int
ebtablesUnlinkRootChain(virConnectPtr conn,
                        virBufferPtr buf,
                        int incoming, const char *ifname)
{
    return _ebtablesUnlinkRootChain(conn, buf, incoming, ifname, 0);
}


static int
ebtablesUnlinkTmpRootChain(virConnectPtr conn,
                           virBufferPtr buf,
                           int incoming, const char *ifname)
{
    return _ebtablesUnlinkRootChain(conn, buf, incoming, ifname, 1);
}


static int
ebtablesCreateTmpSubChain(virConnectPtr conn ATTRIBUTE_UNUSED,
                          virBufferPtr buf,
                          int incoming,
                          const char *ifname,
                          const char *protocol,
                          int stopOnError)
{
    char rootchain[MAX_CHAINNAME_LENGTH], chain[MAX_CHAINNAME_LENGTH];
    char chainPrefix = (incoming) ? CHAINPREFIX_HOST_IN_TEMP
                                  : CHAINPREFIX_HOST_OUT_TEMP;

    PRINT_ROOT_CHAIN(rootchain, chainPrefix, ifname);
    PRINT_CHAIN(chain, chainPrefix, ifname, protocol);

    virBufferVSprintf(buf,
                      CMD_DEF(EBTABLES_CMD " -t %s -N %s") CMD_SEPARATOR
                      CMD_EXEC
                      "%s"
                      CMD_DEF(EBTABLES_CMD " -t %s -A %s -p %s -j %s") CMD_SEPARATOR
                      CMD_EXEC
                      "%s",

                      EBTABLES_DEFAULT_TABLE, chain,

                      CMD_STOPONERR(stopOnError),

                      EBTABLES_DEFAULT_TABLE,
                      rootchain,
                      protocol, chain,

                      CMD_STOPONERR(stopOnError));

    return 0;
}


static int
_ebtablesRemoveSubChain(virConnectPtr conn ATTRIBUTE_UNUSED,
                        virBufferPtr buf,
                        int incoming,
                        const char *ifname,
                        const char *protocol,
                        int isTempChain)
{
    char rootchain[MAX_CHAINNAME_LENGTH], chain[MAX_CHAINNAME_LENGTH];
    char chainPrefix;
    if (isTempChain) {
        chainPrefix =(incoming) ? CHAINPREFIX_HOST_IN_TEMP
                                : CHAINPREFIX_HOST_OUT_TEMP;
    } else {
        chainPrefix =(incoming) ? CHAINPREFIX_HOST_IN
                                : CHAINPREFIX_HOST_OUT;
    }

    PRINT_ROOT_CHAIN(rootchain, chainPrefix, ifname);
    PRINT_CHAIN(chain, chainPrefix, ifname, protocol);

    virBufferVSprintf(buf,
                      EBTABLES_CMD " -t %s -D %s -p %s -j %s" CMD_SEPARATOR
                      EBTABLES_CMD " -t %s -F %s" CMD_SEPARATOR
                      EBTABLES_CMD " -t %s -X %s" CMD_SEPARATOR,
                      EBTABLES_DEFAULT_TABLE,
                      rootchain,
                      protocol, chain,

                      EBTABLES_DEFAULT_TABLE, chain,

                      EBTABLES_DEFAULT_TABLE, chain);

    return 0;
}


static int
ebtablesRemoveSubChain(virConnectPtr conn,
                       virBufferPtr buf,
                       int incoming,
                       const char *ifname,
                       const char *protocol)
{
    return _ebtablesRemoveSubChain(conn, buf,
                                   incoming, ifname, protocol, 0);
}


static int
ebtablesRemoveSubChains(virConnectPtr conn,
                          virBufferPtr buf,
                          const char *ifname)
{
    int i;
    for (i = 0; supported_protocols[i]; i++) {
        ebtablesRemoveSubChain(conn, buf, 1, ifname, supported_protocols[i]);
        ebtablesRemoveSubChain(conn, buf, 0, ifname, supported_protocols[i]);
    }

    return 0;
}


static int
ebtablesRemoveTmpSubChain(virConnectPtr conn,
                          virBufferPtr buf,
                          int incoming,
                          const char *ifname,
                          const char *protocol)
{
    return _ebtablesRemoveSubChain(conn, buf,
                                   incoming, ifname, protocol, 1);
}


static int
ebtablesRemoveTmpSubChains(virConnectPtr conn,
                           virBufferPtr buf,
                           const char *ifname)
{
    int i;
    for (i = 0; supported_protocols[i]; i++) {
        ebtablesRemoveTmpSubChain(conn, buf, 1, ifname,
                                  supported_protocols[i]);
        ebtablesRemoveTmpSubChain(conn, buf, 0, ifname,
                                  supported_protocols[i]);
    }

    return 0;
}


static int
ebtablesRenameTmpSubChain(virConnectPtr conn ATTRIBUTE_UNUSED,
                          virBufferPtr buf,
                          int incoming,
                          const char *ifname,
                          const char *protocol)
{
    char tmpchain[MAX_CHAINNAME_LENGTH], chain[MAX_CHAINNAME_LENGTH];
    char tmpChainPrefix = (incoming) ? CHAINPREFIX_HOST_IN_TEMP
                                     : CHAINPREFIX_HOST_OUT_TEMP;
    char chainPrefix = (incoming) ? CHAINPREFIX_HOST_IN
                                  : CHAINPREFIX_HOST_OUT;

    if (protocol) {
        PRINT_CHAIN(tmpchain, tmpChainPrefix, ifname, protocol);
        PRINT_CHAIN(   chain,    chainPrefix, ifname, protocol);
    } else {
        PRINT_ROOT_CHAIN(tmpchain, tmpChainPrefix, ifname);
        PRINT_ROOT_CHAIN(   chain,    chainPrefix, ifname);
    }

    virBufferVSprintf(buf,
                      EBTABLES_CMD " -t %s -E %s %s" CMD_SEPARATOR,
                      EBTABLES_DEFAULT_TABLE,
                      tmpchain,
                      chain);
    return 0;
}


static int
ebtablesRenameTmpSubChains(virConnectPtr conn,
                           virBufferPtr buf,
                           const char *ifname)
{
    int i;
    for (i = 0; supported_protocols[i]; i++) {
        ebtablesRenameTmpSubChain (conn, buf, 1, ifname,
                                   supported_protocols[i]);
        ebtablesRenameTmpSubChain (conn, buf, 0, ifname,
                                   supported_protocols[i]);
    }

    return 0;
}


static int
ebtablesRenameTmpRootChain(virConnectPtr conn,
                           virBufferPtr buf,
                           int incoming,
                           const char *ifname)
{
    return ebtablesRenameTmpSubChain(conn, buf, incoming, ifname, NULL);
}


static void
ebiptablesInstCommand(virConnectPtr conn ATTRIBUTE_UNUSED,
                      virBufferPtr buf,
                      const char *templ, char cmd, int pos,
                      int stopOnError)
{
    char position[10] = { 0 };
    if (pos >= 0)
        snprintf(position, sizeof(position), "%d", pos);
    virBufferVSprintf(buf, templ, cmd, position);
    virBufferVSprintf(buf, CMD_SEPARATOR "%s",
                      CMD_STOPONERR(stopOnError));
}


static int
ebiptablesRuleOrderSort(const void *a, const void *b)
{
    const ebiptablesRuleInstPtr *insta = a;
    const ebiptablesRuleInstPtr *instb = b;
    return ((*insta)->priority - (*instb)->priority);
}


static int
ebiptablesApplyNewRules(virConnectPtr conn,
                        const char *ifname,
                        int nruleInstances,
                        void **_inst)
{
    int i;
    int cli_status;
    ebiptablesRuleInstPtr *inst = (ebiptablesRuleInstPtr *)_inst;
    int chains_in = 0, chains_out = 0;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    int haveIptables = 0;
    int haveIp6tables = 0;

    if (inst)
        qsort(inst, nruleInstances, sizeof(inst[0]),
              ebiptablesRuleOrderSort);

    for (i = 0; i < nruleInstances; i++) {
        if (inst[i]->ruleType == RT_EBTABLES) {
            if (inst[i]->chainprefix == CHAINPREFIX_HOST_IN_TEMP)
                chains_in  |= (1 << inst[i]->neededProtocolChain);
            else
                chains_out |= (1 << inst[i]->neededProtocolChain);
        }
    }

    ebtablesUnlinkTmpRootChain(conn, &buf, 1, ifname);
    ebtablesUnlinkTmpRootChain(conn, &buf, 0, ifname);
    ebtablesRemoveTmpSubChains(conn, &buf, ifname);
    ebtablesRemoveTmpRootChain(conn, &buf, 1, ifname);
    ebtablesRemoveTmpRootChain(conn, &buf, 0, ifname);
    ebiptablesExecCLI(conn, &buf, &cli_status);

    if (chains_in != 0)
        ebtablesCreateTmpRootChain(conn, &buf, 1, ifname, 1);
    if (chains_out != 0)
        ebtablesCreateTmpRootChain(conn, &buf, 0, ifname, 1);

    if (chains_in  & (1 << VIR_NWFILTER_CHAINSUFFIX_IPv4))
        ebtablesCreateTmpSubChain(conn, &buf, 1, ifname, "ipv4", 1);
    if (chains_out & (1 << VIR_NWFILTER_CHAINSUFFIX_IPv4))
        ebtablesCreateTmpSubChain(conn, &buf, 0, ifname, "ipv4", 1);

    if (chains_in  & (1 << VIR_NWFILTER_CHAINSUFFIX_IPv6))
        ebtablesCreateTmpSubChain(conn, &buf, 1, ifname, "ipv6", 1);
    if (chains_out & (1 << VIR_NWFILTER_CHAINSUFFIX_IPv6))
        ebtablesCreateTmpSubChain(conn, &buf, 0, ifname, "ipv6", 1);

    // keep arp as last
    if (chains_in  & (1 << VIR_NWFILTER_CHAINSUFFIX_ARP))
        ebtablesCreateTmpSubChain(conn, &buf, 1, ifname, "arp", 1);
    if (chains_out & (1 << VIR_NWFILTER_CHAINSUFFIX_ARP))
        ebtablesCreateTmpSubChain(conn, &buf, 0, ifname, "arp", 1);

    if (ebiptablesExecCLI(conn, &buf, &cli_status) || cli_status != 0)
        goto tear_down_tmpebchains;

    for (i = 0; i < nruleInstances; i++)
        switch (inst[i]->ruleType) {
        case RT_EBTABLES:
            ebiptablesInstCommand(conn, &buf,
                                  inst[i]->commandTemplate,
                                  'A', -1, 1);
        break;
        case RT_IPTABLES:
            haveIptables = 1;
        break;
        case RT_IP6TABLES:
            haveIp6tables = 1;
        break;
        }

    if (ebiptablesExecCLI(conn, &buf, &cli_status) || cli_status != 0)
        goto tear_down_tmpebchains;

    // FIXME: establishment of iptables user define table tree goes here

    if (haveIptables) {
        iptablesUnlinkTmpRootChains(conn, IPTABLES_CMD, &buf, ifname);
        iptablesRemoveTmpRootChains(conn, IPTABLES_CMD, &buf, ifname);

        iptablesCreateBaseChains(conn, IPTABLES_CMD, &buf);

        if (ebiptablesExecCLI(conn, &buf, &cli_status) || cli_status != 0)
            goto tear_down_tmpebchains;

        iptablesCreateTmpRootChains(conn, IPTABLES_CMD, &buf, ifname);

        if (ebiptablesExecCLI(conn, &buf, &cli_status) || cli_status != 0)
           goto tear_down_tmpiptchains;

        iptablesLinkTmpRootChains(conn, IPTABLES_CMD, &buf, ifname);
        iptablesSetupVirtInPost(conn, IPTABLES_CMD, &buf, ifname);
        if (ebiptablesExecCLI(conn, &buf, &cli_status) || cli_status != 0)
           goto tear_down_tmpiptchains;

        for (i = 0; i < nruleInstances; i++) {
            if (inst[i]->ruleType == RT_IPTABLES)
                iptablesInstCommand(conn, &buf,
                                    inst[i]->commandTemplate,
                                    'A', -1, 1);
        }

        if (ebiptablesExecCLI(conn, &buf, &cli_status) || cli_status != 0)
           goto tear_down_tmpiptchains;
    }

    if (haveIp6tables) {
        iptablesUnlinkTmpRootChains(conn, IP6TABLES_CMD, &buf, ifname);
        iptablesRemoveTmpRootChains(conn, IP6TABLES_CMD, &buf, ifname);

        iptablesCreateBaseChains(conn, IP6TABLES_CMD, &buf);

        if (ebiptablesExecCLI(conn, &buf, &cli_status) || cli_status != 0)
            goto tear_down_tmpiptchains;

        iptablesCreateTmpRootChains(conn, IP6TABLES_CMD, &buf, ifname);

        if (ebiptablesExecCLI(conn, &buf, &cli_status) || cli_status != 0)
           goto tear_down_tmpip6tchains;

        iptablesLinkTmpRootChains(conn, IP6TABLES_CMD, &buf, ifname);
        iptablesSetupVirtInPost(conn, IP6TABLES_CMD, &buf, ifname);
        if (ebiptablesExecCLI(conn, &buf, &cli_status) || cli_status != 0)
           goto tear_down_tmpip6tchains;

        for (i = 0; i < nruleInstances; i++) {
            if (inst[i]->ruleType == RT_IP6TABLES)
                iptablesInstCommand(conn, &buf,
                                    inst[i]->commandTemplate,
                                    'A', -1, 1);
        }

        if (ebiptablesExecCLI(conn, &buf, &cli_status) || cli_status != 0)
           goto tear_down_tmpip6tchains;
    }


    // END IPTABLES stuff

    if (chains_in != 0)
        ebtablesLinkTmpRootChain(conn, &buf, 1, ifname, 1);
    if (chains_out != 0)
        ebtablesLinkTmpRootChain(conn, &buf, 0, ifname, 1);

    if (ebiptablesExecCLI(conn, &buf, &cli_status) || cli_status != 0)
        goto tear_down_ebsubchains_and_unlink;

    return 0;

tear_down_ebsubchains_and_unlink:
    ebtablesUnlinkTmpRootChain(conn, &buf, 1, ifname);
    ebtablesUnlinkTmpRootChain(conn, &buf, 0, ifname);

tear_down_tmpip6tchains:
    if (haveIp6tables) {
        iptablesUnlinkTmpRootChains(conn, IP6TABLES_CMD, &buf, ifname);
        iptablesRemoveTmpRootChains(conn, IP6TABLES_CMD, &buf, ifname);
    }

tear_down_tmpiptchains:
    if (haveIptables) {
        iptablesUnlinkTmpRootChains(conn, IPTABLES_CMD, &buf, ifname);
        iptablesRemoveTmpRootChains(conn, IPTABLES_CMD, &buf, ifname);
    }

tear_down_tmpebchains:
    ebtablesRemoveTmpSubChains(conn, &buf, ifname);
    ebtablesRemoveTmpRootChain(conn, &buf, 1, ifname);
    ebtablesRemoveTmpRootChain(conn, &buf, 0, ifname);

    ebiptablesExecCLI(conn, &buf, &cli_status);

    virNWFilterReportError(conn, VIR_ERR_BUILD_FIREWALL,
                           "%s",
                           _("Some rules could not be created."));

    return 1;
}


static int
ebiptablesTearNewRules(virConnectPtr conn,
                       const char *ifname)
{
    int cli_status;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    iptablesUnlinkTmpRootChains(conn, IPTABLES_CMD, &buf, ifname);
    iptablesRemoveTmpRootChains(conn, IPTABLES_CMD, &buf, ifname);

    iptablesUnlinkTmpRootChains(conn, IP6TABLES_CMD, &buf, ifname);
    iptablesRemoveTmpRootChains(conn, IP6TABLES_CMD, &buf, ifname);

    ebtablesUnlinkTmpRootChain(conn, &buf, 1, ifname);
    ebtablesUnlinkTmpRootChain(conn, &buf, 0, ifname);

    ebtablesRemoveTmpSubChains(conn, &buf, ifname);
    ebtablesRemoveTmpRootChain(conn, &buf, 1, ifname);
    ebtablesRemoveTmpRootChain(conn, &buf, 0, ifname);

    ebiptablesExecCLI(conn, &buf, &cli_status);

    return 0;
}


static int
ebiptablesTearOldRules(virConnectPtr conn,
                       const char *ifname)
{
    int cli_status;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    // switch to new iptables user defined chains
    iptablesUnlinkRootChains(conn, IPTABLES_CMD, &buf, ifname);
    iptablesRemoveRootChains(conn, IPTABLES_CMD, &buf, ifname);

    iptablesRenameTmpRootChains(conn, IPTABLES_CMD, &buf, ifname);
    ebiptablesExecCLI(conn, &buf, &cli_status);

    iptablesUnlinkRootChains(conn, IP6TABLES_CMD, &buf, ifname);
    iptablesRemoveRootChains(conn, IP6TABLES_CMD, &buf, ifname);

    iptablesRenameTmpRootChains(conn, IP6TABLES_CMD, &buf, ifname);
    ebiptablesExecCLI(conn, &buf, &cli_status);

    ebtablesUnlinkRootChain(conn, &buf, 1, ifname);
    ebtablesUnlinkRootChain(conn, &buf, 0, ifname);

    ebtablesRemoveSubChains(conn, &buf, ifname);

    ebtablesRemoveRootChain(conn, &buf, 1, ifname);
    ebtablesRemoveRootChain(conn, &buf, 0, ifname);

    ebtablesRenameTmpSubChains(conn, &buf, ifname);
    ebtablesRenameTmpRootChain(conn, &buf, 1, ifname);
    ebtablesRenameTmpRootChain(conn, &buf, 0, ifname);

    ebiptablesExecCLI(conn, &buf, &cli_status);

    return 0;
}


/**
 * ebiptablesRemoveRules:
 * @conn : pointer to virConnect object
 * @ifname : the name of the interface to which the rules apply
 * @nRuleInstance : the number of given rules
 * @_inst : array of rule instantiation data
 *
 * Remove all rules one after the other
 *
 * Return 0 on success, 1 if execution of one or more cleanup
 * commands failed.
 */
static int
ebiptablesRemoveRules(virConnectPtr conn,
                      const char *ifname ATTRIBUTE_UNUSED,
                      int nruleInstances,
                      void **_inst)
{
    int rc = 0;
    int cli_status;
    int i;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    ebiptablesRuleInstPtr *inst = (ebiptablesRuleInstPtr *)_inst;

    for (i = 0; i < nruleInstances; i++)
        ebiptablesInstCommand(conn, &buf,
                              inst[i]->commandTemplate,
                              'D', -1,
                              0);

    if (ebiptablesExecCLI(conn, &buf, &cli_status))
        goto err_exit;

    if (cli_status) {
        virNWFilterReportError(conn, VIR_ERR_BUILD_FIREWALL,
                               "%s",
                               _("error while executing CLI commands"));
        rc = 1;
    }

err_exit:
    return rc;
}


/**
 * ebiptablesAllTeardown:
 * @ifname : the name of the interface to which the rules apply
 *
 * Unconditionally remove all possible user defined tables and rules
 * that were created for the given interface (ifname).
 *
 * Always returns 0.
 */
static int
ebiptablesAllTeardown(const char *ifname)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    int cli_status;
    virConnectPtr conn = NULL;

    iptablesUnlinkRootChains(conn, IPTABLES_CMD, &buf, ifname);
    iptablesClearVirtInPost (conn, IPTABLES_CMD, &buf, ifname);
    iptablesRemoveRootChains(conn, IPTABLES_CMD, &buf, ifname);

    iptablesUnlinkRootChains(conn, IP6TABLES_CMD, &buf, ifname);
    iptablesClearVirtInPost (conn, IP6TABLES_CMD, &buf, ifname);
    iptablesRemoveRootChains(conn, IP6TABLES_CMD, &buf, ifname);

    ebtablesUnlinkRootChain(conn, &buf, 1, ifname);
    ebtablesUnlinkRootChain(conn, &buf, 0, ifname);

    ebtablesRemoveRootChain(conn, &buf, 1, ifname);
    ebtablesRemoveRootChain(conn, &buf, 0, ifname);

    ebtablesRemoveSubChains(conn, &buf, ifname);

    ebiptablesExecCLI(conn, &buf, &cli_status);

    return 0;
}


virNWFilterTechDriver ebiptables_driver = {
    .name = EBIPTABLES_DRIVER_ID,

    .createRuleInstance  = ebiptablesCreateRuleInstance,
    .applyNewRules       = ebiptablesApplyNewRules,
    .tearNewRules        = ebiptablesTearNewRules,
    .tearOldRules        = ebiptablesTearOldRules,
    .allTeardown         = ebiptablesAllTeardown,
    .removeRules         = ebiptablesRemoveRules,
    .freeRuleInstance    = ebiptablesFreeRuleInstance,
    .displayRuleInstance = ebiptablesDisplayRuleInstance,
};