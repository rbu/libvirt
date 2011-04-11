/*
 * network_conf.c: network XML handling
 *
 * Copyright (C) 2006-2010 Red Hat, Inc.
 * Copyright (C) 2006-2008 Daniel P. Berrange
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
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */



#include <config.h>

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <dirent.h>

#include "virterror_internal.h"
#include "datatypes.h"
#include "network_conf.h"
#include "network.h"
#include "memory.h"
#include "xml.h"
#include "uuid.h"
#include "util.h"
#include "buf.h"
#include "c-ctype.h"

#define MAX_BRIDGE_ID 256
#define VIR_FROM_THIS VIR_FROM_NETWORK

VIR_ENUM_DECL(virNetworkForward)

VIR_ENUM_IMPL(virNetworkForward,
              VIR_NETWORK_FORWARD_LAST,
              "none", "nat", "route" )

#define virNetworkReportError(code, ...)                                \
    virReportErrorHelper(NULL, VIR_FROM_NETWORK, code, __FILE__,        \
                         __FUNCTION__, __LINE__, __VA_ARGS__)

virNetworkObjPtr virNetworkFindByUUID(const virNetworkObjListPtr nets,
                                      const unsigned char *uuid)
{
    unsigned int i;

    for (i = 0 ; i < nets->count ; i++) {
        virNetworkObjLock(nets->objs[i]);
        if (!memcmp(nets->objs[i]->def->uuid, uuid, VIR_UUID_BUFLEN))
            return nets->objs[i];
        virNetworkObjUnlock(nets->objs[i]);
    }

    return NULL;
}

virNetworkObjPtr virNetworkFindByName(const virNetworkObjListPtr nets,
                                      const char *name)
{
    unsigned int i;

    for (i = 0 ; i < nets->count ; i++) {
        virNetworkObjLock(nets->objs[i]);
        if (STREQ(nets->objs[i]->def->name, name))
            return nets->objs[i];
        virNetworkObjUnlock(nets->objs[i]);
    }

    return NULL;
}


void virNetworkDefFree(virNetworkDefPtr def)
{
    int i;

    if (!def)
        return;

    VIR_FREE(def->name);
    VIR_FREE(def->bridge);
    VIR_FREE(def->forwardDev);
    VIR_FREE(def->ipAddress);
    VIR_FREE(def->network);
    VIR_FREE(def->netmask);
    VIR_FREE(def->domain);

    for (i = 0 ; i < def->nranges && def->ranges ; i++) {
        VIR_FREE(def->ranges[i].start);
        VIR_FREE(def->ranges[i].end);
    }
    VIR_FREE(def->ranges);

    for (i = 0 ; i < def->nhosts && def->hosts ; i++) {
        VIR_FREE(def->hosts[i].mac);
        VIR_FREE(def->hosts[i].ip);
        VIR_FREE(def->hosts[i].name);
    }
    VIR_FREE(def->hosts);

    VIR_FREE(def->tftproot);
    VIR_FREE(def->bootfile);
    VIR_FREE(def->bootserver);

    VIR_FREE(def);
}

void virNetworkObjFree(virNetworkObjPtr net)
{
    if (!net)
        return;

    virNetworkDefFree(net->def);
    virNetworkDefFree(net->newDef);

    virMutexDestroy(&net->lock);

    VIR_FREE(net);
}

void virNetworkObjListFree(virNetworkObjListPtr nets)
{
    unsigned int i;

    for (i = 0 ; i < nets->count ; i++)
        virNetworkObjFree(nets->objs[i]);

    VIR_FREE(nets->objs);
    nets->count = 0;
}

virNetworkObjPtr virNetworkAssignDef(virNetworkObjListPtr nets,
                                     const virNetworkDefPtr def)
{
    virNetworkObjPtr network;

    if ((network = virNetworkFindByName(nets, def->name))) {
        if (!virNetworkObjIsActive(network)) {
            virNetworkDefFree(network->def);
            network->def = def;
        } else {
            virNetworkDefFree(network->newDef);
            network->newDef = def;
        }

        return network;
    }

    if (VIR_ALLOC(network) < 0) {
        virReportOOMError();
        return NULL;
    }
    if (virMutexInit(&network->lock) < 0) {
        virNetworkReportError(VIR_ERR_INTERNAL_ERROR,
                              "%s", _("cannot initialize mutex"));
        VIR_FREE(network);
        return NULL;
    }
    virNetworkObjLock(network);
    network->def = def;

    if (VIR_REALLOC_N(nets->objs, nets->count + 1) < 0) {
        virReportOOMError();
        VIR_FREE(network);
        return NULL;
    }

    nets->objs[nets->count] = network;
    nets->count++;

    return network;

}

void virNetworkRemoveInactive(virNetworkObjListPtr nets,
                              const virNetworkObjPtr net)
{
    unsigned int i;

    virNetworkObjUnlock(net);
    for (i = 0 ; i < nets->count ; i++) {
        virNetworkObjLock(nets->objs[i]);
        if (nets->objs[i] == net) {
            virNetworkObjUnlock(nets->objs[i]);
            virNetworkObjFree(nets->objs[i]);

            if (i < (nets->count - 1))
                memmove(nets->objs + i, nets->objs + i + 1,
                        sizeof(*(nets->objs)) * (nets->count - (i + 1)));

            if (VIR_REALLOC_N(nets->objs, nets->count - 1) < 0) {
                ; /* Failure to reduce memory allocation isn't fatal */
            }
            nets->count--;

            break;
        }
        virNetworkObjUnlock(nets->objs[i]);
    }
}


static int
virNetworkDHCPRangeDefParseXML(virNetworkDefPtr def,
                               xmlNodePtr node) {

    xmlNodePtr cur;

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE &&
            xmlStrEqual(cur->name, BAD_CAST "range")) {
            char *start, *end;
            virSocketAddr saddr, eaddr;
            int range;

            if (!(start = (char *) xmlGetProp(cur, BAD_CAST "start"))) {
                cur = cur->next;
                continue;
            }
            if (!(end = (char *) xmlGetProp(cur, BAD_CAST "end"))) {
                xmlFree(start);
                cur = cur->next;
                continue;
            }

            if (virSocketParseAddr(start, &saddr, 0) < 0) {
                virNetworkReportError(VIR_ERR_XML_ERROR,
                                      _("cannot parse dhcp start address '%s'"),
                                      start);
                xmlFree(start);
                xmlFree(end);
                cur = cur->next;
                continue;
            }
            if (virSocketParseAddr(end, &eaddr, 0) < 0) {
                virNetworkReportError(VIR_ERR_XML_ERROR,
                                      _("cannot parse dhcp end address '%s'"),
                                      end);
                xmlFree(start);
                xmlFree(end);
                cur = cur->next;
                continue;
            }

            range = virSocketGetRange(&saddr, &eaddr);
            if (range < 0) {
                virNetworkReportError(VIR_ERR_XML_ERROR,
                                      _("dhcp range '%s' to '%s' invalid"),
                                      start, end);
                xmlFree(start);
                xmlFree(end);
                cur = cur->next;
                continue;
            }

            if (VIR_REALLOC_N(def->ranges, def->nranges + 1) < 0) {
                xmlFree(start);
                xmlFree(end);
                virReportOOMError();
                return -1;
            }
            def->ranges[def->nranges].start = (char *)start;
            def->ranges[def->nranges].end = (char *)end;
            def->ranges[def->nranges].size = range;
            def->nranges++;
        } else if (cur->type == XML_ELEMENT_NODE &&
            xmlStrEqual(cur->name, BAD_CAST "host")) {
            xmlChar *mac, *name, *ip;
            unsigned char addr[6];
            struct in_addr inaddress;

            mac = xmlGetProp(cur, BAD_CAST "mac");
            if ((mac != NULL) &&
                (virParseMacAddr((const char *) mac, &addr[0]) != 0)) {
                virNetworkReportError(VIR_ERR_INTERNAL_ERROR,
                                      _("cannot parse MAC address '%s'"),
                                      mac);
                VIR_FREE(mac);
            }
            name = xmlGetProp(cur, BAD_CAST "name");
            if ((name != NULL) && (!c_isalpha(name[0]))) {
                virNetworkReportError(VIR_ERR_INTERNAL_ERROR,
                                      _("cannot use name address '%s'"),
                                      name);
                VIR_FREE(name);
            }
            /*
             * You need at least one MAC address or one host name
             */
            if ((mac == NULL) && (name == NULL)) {
                VIR_FREE(mac);
                VIR_FREE(name);
                cur = cur->next;
                continue;
            }
            ip = xmlGetProp(cur, BAD_CAST "ip");
            if (inet_pton(AF_INET, (const char *) ip, &inaddress) <= 0) {
                virNetworkReportError(VIR_ERR_INTERNAL_ERROR,
                                      _("cannot parse IP address '%s'"),
                                      ip);
                VIR_FREE(ip);
                VIR_FREE(mac);
                VIR_FREE(name);
                cur = cur->next;
                continue;
            }
            if (VIR_REALLOC_N(def->hosts, def->nhosts + 1) < 0) {
                VIR_FREE(ip);
                VIR_FREE(mac);
                VIR_FREE(name);
                virReportOOMError();
                return -1;
            }
            def->hosts[def->nhosts].mac = (char *)mac;
            def->hosts[def->nhosts].name = (char *)name;
            def->hosts[def->nhosts].ip = (char *)ip;
            def->nhosts++;

        } else if (cur->type == XML_ELEMENT_NODE &&
            xmlStrEqual(cur->name, BAD_CAST "bootp")) {
            xmlChar *file;

            if (!(file = xmlGetProp(cur, BAD_CAST "file"))) {
                cur = cur->next;
                continue;
            }

            def->bootfile = (char *)file;
            def->bootserver = (char *) xmlGetProp(cur, BAD_CAST "server");
        }

        cur = cur->next;
    }

    return 0;
}

static int
virNetworkIPParseXML(virNetworkDefPtr def,
                     xmlNodePtr node) {
    xmlNodePtr cur;

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE &&
            xmlStrEqual(cur->name, BAD_CAST "dhcp")) {
            int result = virNetworkDHCPRangeDefParseXML(def, cur);
            if (result)
                return result;

        } else if (cur->type == XML_ELEMENT_NODE &&
            xmlStrEqual(cur->name, BAD_CAST "tftp")) {
            xmlChar *root;

            if (!(root = xmlGetProp(cur, BAD_CAST "root"))) {
                cur = cur->next;
                continue;
            }

            def->tftproot = (char *)root;
        }

        cur = cur->next;
    }
    return 0;
}

static virNetworkDefPtr
virNetworkDefParseXML(xmlXPathContextPtr ctxt)
{
    virNetworkDefPtr def;
    char *tmp;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    /* Extract network name */
    def->name = virXPathString("string(./name[1])", ctxt);
    if (!def->name) {
        virNetworkReportError(VIR_ERR_NO_NAME, NULL);
        goto error;
    }

    /* Extract network uuid */
    tmp = virXPathString("string(./uuid[1])", ctxt);
    if (!tmp) {
        if (virUUIDGenerate(def->uuid)) {
            virNetworkReportError(VIR_ERR_INTERNAL_ERROR,
                                  "%s", _("Failed to generate UUID"));
            goto error;
        }
    } else {
        if (virUUIDParse(tmp, def->uuid) < 0) {
            VIR_FREE(tmp);
            virNetworkReportError(VIR_ERR_INTERNAL_ERROR,
                                  "%s", _("malformed uuid element"));
            goto error;
        }
        VIR_FREE(tmp);
    }

    /* Parse network domain information */
    def->domain = virXPathString("string(./domain[1]/@name)", ctxt);

    /* Parse bridge information */
    def->bridge = virXPathString("string(./bridge[1]/@name)", ctxt);
    tmp = virXPathString("string(./bridge[1]/@stp)", ctxt);
    def->stp = (tmp && STREQ(tmp, "off")) ? 0 : 1;
    VIR_FREE(tmp);

    if (virXPathULong("string(./bridge[1]/@delay)", ctxt, &def->delay) < 0)
        def->delay = 0;

    def->ipAddress = virXPathString("string(./ip[1]/@address)", ctxt);
    def->netmask = virXPathString("string(./ip[1]/@netmask)", ctxt);
    if (def->ipAddress &&
        def->netmask) {
        /* XXX someday we want IPv6 too, so inet_aton won't work there */
        struct in_addr inaddress, innetmask;
        char *netaddr;
        xmlNodePtr ip;

        if (inet_pton(AF_INET, def->ipAddress, &inaddress) <= 0) {
            virNetworkReportError(VIR_ERR_INTERNAL_ERROR,
                                  _("cannot parse IP address '%s'"),
                                  def->ipAddress);
            goto error;
        }
        if (inet_pton(AF_INET, def->netmask, &innetmask) <= 0) {
            virNetworkReportError(VIR_ERR_INTERNAL_ERROR,
                                  _("cannot parse netmask '%s'"),
                                  def->netmask);
            goto error;
        }

        inaddress.s_addr &= innetmask.s_addr;
        netaddr = inet_ntoa(inaddress);

        if (virAsprintf(&def->network, "%s/%s", netaddr, def->netmask) < 0) {
            virReportOOMError();
            goto error;
        }

        if ((ip = virXPathNode("./ip[1]", ctxt)) &&
            virNetworkIPParseXML(def, ip) < 0)
            goto error;
    }


    /* IPv4 forwarding setup */
    if (virXPathBoolean("count(./forward) > 0", ctxt)) {
        if (!def->ipAddress ||
            !def->netmask) {
            virNetworkReportError(VIR_ERR_INTERNAL_ERROR,
                                  "%s", _("Forwarding requested, but no IPv4 address/netmask provided"));
            goto error;
        }

        tmp = virXPathString("string(./forward[1]/@mode)", ctxt);
        if (tmp) {
            if ((def->forwardType = virNetworkForwardTypeFromString(tmp)) < 0) {
                virNetworkReportError(VIR_ERR_INTERNAL_ERROR,
                                      _("unknown forwarding type '%s'"), tmp);
                VIR_FREE(tmp);
                goto error;
            }
            VIR_FREE(tmp);
        } else {
            def->forwardType = VIR_NETWORK_FORWARD_NAT;
        }

        tmp = virXPathString("string(./forward[1]/@adjustFirewall)", ctxt);
        if (tmp) {
            def->adjustFirewall = (STREQ(tmp, "off") ? 0 : 1);
        } else {
            def->adjustFirewall = 1;
        }
        VIR_FREE(tmp);

        def->forwardDev = virXPathString("string(./forward[1]/@dev)", ctxt);
    } else {
        def->forwardType = VIR_NETWORK_FORWARD_NONE;
    }

    return def;

 error:
    virNetworkDefFree(def);
    return NULL;
}

static virNetworkDefPtr
virNetworkDefParse(const char *xmlStr,
                   const char *filename)
{
    xmlDocPtr xml;
    virNetworkDefPtr def = NULL;

    if ((xml = virXMLParse(filename, xmlStr, "network.xml"))) {
        def = virNetworkDefParseNode(xml, xmlDocGetRootElement(xml));
        xmlFreeDoc(xml);
    }

    return def;
}

virNetworkDefPtr virNetworkDefParseString(const char *xmlStr)
{
    return virNetworkDefParse(xmlStr, NULL);
}

virNetworkDefPtr virNetworkDefParseFile(const char *filename)
{
    return virNetworkDefParse(NULL, filename);
}


virNetworkDefPtr virNetworkDefParseNode(xmlDocPtr xml,
                                        xmlNodePtr root)
{
    xmlXPathContextPtr ctxt = NULL;
    virNetworkDefPtr def = NULL;

    if (!xmlStrEqual(root->name, BAD_CAST "network")) {
        virNetworkReportError(VIR_ERR_INTERNAL_ERROR,
                              "%s", _("incorrect root element"));
        return NULL;
    }

    ctxt = xmlXPathNewContext(xml);
    if (ctxt == NULL) {
        virReportOOMError();
        goto cleanup;
    }

    ctxt->node = root;
    def = virNetworkDefParseXML(ctxt);

cleanup:
    xmlXPathFreeContext(ctxt);
    return def;
}

char *virNetworkDefFormat(const virNetworkDefPtr def)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    unsigned char *uuid;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    virBufferAddLit(&buf, "<network>\n");
    virBufferEscapeString(&buf, "  <name>%s</name>\n", def->name);

    uuid = def->uuid;
    virUUIDFormat(uuid, uuidstr);
    virBufferVSprintf(&buf, "  <uuid>%s</uuid>\n", uuidstr);

    if (def->forwardType != VIR_NETWORK_FORWARD_NONE) {
        const char *mode = virNetworkForwardTypeToString(def->forwardType);
        if (mode) {
            if (def->forwardDev) {
                virBufferEscapeString(&buf, "  <forward dev='%s'",
                                      def->forwardDev);
            } else {
                virBufferAddLit(&buf, "  <forward");
            }
            virBufferVSprintf(&buf, " mode='%s'", mode);
            virBufferVSprintf(&buf, " adjustFirewall='%s'", def->adjustFirewall ? "on" : "off");
            virBufferAddLit(&buf, "/>\n");
        }
    }

    virBufferAddLit(&buf, "  <bridge");
    if (def->bridge)
        virBufferEscapeString(&buf, " name='%s'", def->bridge);
    virBufferVSprintf(&buf, " stp='%s' delay='%ld' />\n",
                      def->stp ? "on" : "off",
                      def->delay);

    if (def->domain)
        virBufferVSprintf(&buf, "  <domain name='%s'/>\n", def->domain);

    if (def->ipAddress || def->netmask) {
        virBufferAddLit(&buf, "  <ip");

        if (def->ipAddress)
            virBufferVSprintf(&buf, " address='%s'", def->ipAddress);

        if (def->netmask)
            virBufferVSprintf(&buf, " netmask='%s'", def->netmask);

        virBufferAddLit(&buf, ">\n");

        if (def->tftproot) {
            virBufferEscapeString(&buf, "    <tftp root='%s' />\n",
                                  def->tftproot);
        }
        if ((def->nranges || def->nhosts)) {
            int i;
            virBufferAddLit(&buf, "    <dhcp>\n");
            for (i = 0 ; i < def->nranges ; i++)
                virBufferVSprintf(&buf, "      <range start='%s' end='%s' />\n",
                                  def->ranges[i].start, def->ranges[i].end);
            for (i = 0 ; i < def->nhosts ; i++) {
                virBufferAddLit(&buf, "      <host ");
                if (def->hosts[i].mac)
                    virBufferVSprintf(&buf, "mac='%s' ", def->hosts[i].mac);
                if (def->hosts[i].name)
                    virBufferVSprintf(&buf, "name='%s' ", def->hosts[i].name);
                if (def->hosts[i].ip)
                    virBufferVSprintf(&buf, "ip='%s' ", def->hosts[i].ip);
                virBufferAddLit(&buf, "/>\n");
            }
            if (def->bootfile) {
                virBufferEscapeString(&buf, "      <bootp file='%s' ",
                                      def->bootfile);
                if (def->bootserver) {
                    virBufferEscapeString(&buf, "server='%s' ",
                                          def->bootserver);
                }
                virBufferAddLit(&buf, "/>\n");
            }

            virBufferAddLit(&buf, "    </dhcp>\n");
        }

        virBufferAddLit(&buf, "  </ip>\n");
    }

    virBufferAddLit(&buf, "</network>\n");

    if (virBufferError(&buf))
        goto no_memory;

    return virBufferContentAndReset(&buf);

 no_memory:
    virReportOOMError();
    virBufferFreeAndReset(&buf);
    return NULL;
}

int virNetworkSaveXML(const char *configDir,
                      virNetworkDefPtr def,
                      const char *xml)
{
    char *configFile = NULL;
    int fd = -1, ret = -1;
    size_t towrite;
    int err;

    if ((configFile = virNetworkConfigFile(configDir, def->name)) == NULL)
        goto cleanup;

    if ((err = virFileMakePath(configDir))) {
        virReportSystemError(err,
                             _("cannot create config directory '%s'"),
                             configDir);
        goto cleanup;
    }

    if ((fd = open(configFile,
                   O_WRONLY | O_CREAT | O_TRUNC,
                   S_IRUSR | S_IWUSR )) < 0) {
        virReportSystemError(errno,
                             _("cannot create config file '%s'"),
                             configFile);
        goto cleanup;
    }

    towrite = strlen(xml);
    if (safewrite(fd, xml, towrite) < 0) {
        virReportSystemError(errno,
                             _("cannot write config file '%s'"),
                             configFile);
        goto cleanup;
    }

    if (close(fd) < 0) {
        virReportSystemError(errno,
                             _("cannot save config file '%s'"),
                             configFile);
        goto cleanup;
    }

    ret = 0;

 cleanup:
    if (fd != -1)
        close(fd);

    VIR_FREE(configFile);

    return ret;
}

int virNetworkSaveConfig(const char *configDir,
                         virNetworkDefPtr def)
{
    int ret = -1;
    char *xml;

    if (!(xml = virNetworkDefFormat(def)))
        goto cleanup;

    if (virNetworkSaveXML(configDir, def, xml))
        goto cleanup;

    ret = 0;
cleanup:
    VIR_FREE(xml);
    return ret;
}


virNetworkObjPtr virNetworkLoadConfig(virNetworkObjListPtr nets,
                                      const char *configDir,
                                      const char *autostartDir,
                                      const char *name)
{
    char *configFile = NULL, *autostartLink = NULL;
    virNetworkDefPtr def = NULL;
    virNetworkObjPtr net;
    int autostart;

    if ((configFile = virNetworkConfigFile(configDir, name)) == NULL)
        goto error;
    if ((autostartLink = virNetworkConfigFile(autostartDir, name)) == NULL)
        goto error;

    if ((autostart = virFileLinkPointsTo(autostartLink, configFile)) < 0)
        goto error;

    if (!(def = virNetworkDefParseFile(configFile)))
        goto error;

    if (!STREQ(name, def->name)) {
        virNetworkReportError(VIR_ERR_INTERNAL_ERROR,
                              _("Network config filename '%s'"
                                " does not match network name '%s'"),
                              configFile, def->name);
        goto error;
    }

    /* Generate a bridge if none is specified, but don't check for collisions
     * if a bridge is hardcoded, so the network is at least defined
     */
    if (virNetworkSetBridgeName(nets, def, 0))
        goto error;

    if (!(net = virNetworkAssignDef(nets, def)))
        goto error;

    net->autostart = autostart;
    net->persistent = 1;

    VIR_FREE(configFile);
    VIR_FREE(autostartLink);

    return net;

error:
    VIR_FREE(configFile);
    VIR_FREE(autostartLink);
    virNetworkDefFree(def);
    return NULL;
}

int virNetworkLoadAllConfigs(virNetworkObjListPtr nets,
                             const char *configDir,
                             const char *autostartDir)
{
    DIR *dir;
    struct dirent *entry;

    if (!(dir = opendir(configDir))) {
        if (errno == ENOENT)
            return 0;
        virReportSystemError(errno,
                             _("Failed to open dir '%s'"),
                             configDir);
        return -1;
    }

    while ((entry = readdir(dir))) {
        virNetworkObjPtr net;

        if (entry->d_name[0] == '.')
            continue;

        if (!virFileStripSuffix(entry->d_name, ".xml"))
            continue;

        /* NB: ignoring errors, so one malformed config doesn't
           kill the whole process */
        net = virNetworkLoadConfig(nets,
                                   configDir,
                                   autostartDir,
                                   entry->d_name);
        if (net)
            virNetworkObjUnlock(net);
    }

    closedir(dir);

    return 0;
}

int virNetworkDeleteConfig(const char *configDir,
                           const char *autostartDir,
                           virNetworkObjPtr net)
{
    char *configFile = NULL;
    char *autostartLink = NULL;
    int ret = -1;

    if ((configFile = virNetworkConfigFile(configDir, net->def->name)) == NULL)
        goto error;
    if ((autostartLink = virNetworkConfigFile(autostartDir, net->def->name)) == NULL)
        goto error;

    /* Not fatal if this doesn't work */
    unlink(autostartLink);

    if (unlink(configFile) < 0) {
        virReportSystemError(errno,
                             _("cannot remove config file '%s'"),
                             configFile);
        goto error;
    }

    ret = 0;

error:
    VIR_FREE(configFile);
    VIR_FREE(autostartLink);
    return ret;
}

char *virNetworkConfigFile(const char *dir,
                           const char *name)
{
    char *ret = NULL;

    if (virAsprintf(&ret, "%s/%s.xml", dir, name) < 0) {
        virReportOOMError();
        return NULL;
    }

    return ret;
}

int virNetworkBridgeInUse(const virNetworkObjListPtr nets,
                          const char *bridge,
                          const char *skipname)
{
    unsigned int i;
    unsigned int ret = 0;

    for (i = 0 ; i < nets->count ; i++) {
        virNetworkObjLock(nets->objs[i]);
        if (nets->objs[i]->def->bridge &&
            STREQ(nets->objs[i]->def->bridge, bridge) &&
            !(skipname && STREQ(nets->objs[i]->def->name, skipname)))
                ret = 1;
        virNetworkObjUnlock(nets->objs[i]);
    }

    return ret;
}

char *virNetworkAllocateBridge(const virNetworkObjListPtr nets,
                               const char *template)
{

    int id = 0;
    char *newname;

    if (!template)
        template = "virbr%d";

    do {
        char try[50];

        snprintf(try, sizeof(try), template, id);

        if (!virNetworkBridgeInUse(nets, try, NULL)) {
            if (!(newname = strdup(try))) {
                virReportOOMError();
                return NULL;
            }
            return newname;
        }

        id++;
    } while (id <= MAX_BRIDGE_ID);

    virNetworkReportError(VIR_ERR_INTERNAL_ERROR,
                          _("Bridge generation exceeded max id %d"),
                          MAX_BRIDGE_ID);
    return NULL;
}

int virNetworkSetBridgeName(const virNetworkObjListPtr nets,
                            virNetworkDefPtr def,
                            int check_collision) {

    int ret = -1;

    if (def->bridge && !strstr(def->bridge, "%d")) {
        /* We may want to skip collision detection in this case (ex. when
         * loading configs at daemon startup, so the network is at least
         * defined. */
        if (check_collision &&
            virNetworkBridgeInUse(nets, def->bridge, def->name)) {
            virNetworkReportError(VIR_ERR_INTERNAL_ERROR,
                                  _("bridge name '%s' already in use."),
                                  def->bridge);
            goto error;
        }
    } else {
        /* Allocate a bridge name */
        if (!(def->bridge = virNetworkAllocateBridge(nets, def->bridge)))
            goto error;
    }

    ret = 0;
error:
    return ret;
}


/*
 * virNetworkObjIsDuplicate:
 * @doms : virNetworkObjListPtr to search
 * @def  : virNetworkDefPtr definition of network to lookup
 * @check_active: If true, ensure that network is not active
 *
 * Returns: -1 on error
 *          0 if network is new
 *          1 if network is a duplicate
 */
int
virNetworkObjIsDuplicate(virNetworkObjListPtr doms,
                         virNetworkDefPtr def,
                         unsigned int check_active)
{
    int ret = -1;
    int dupVM = 0;
    virNetworkObjPtr vm = NULL;

    /* See if a VM with matching UUID already exists */
    vm = virNetworkFindByUUID(doms, def->uuid);
    if (vm) {
        /* UUID matches, but if names don't match, refuse it */
        if (STRNEQ(vm->def->name, def->name)) {
            char uuidstr[VIR_UUID_STRING_BUFLEN];
            virUUIDFormat(vm->def->uuid, uuidstr);
            virNetworkReportError(VIR_ERR_OPERATION_FAILED,
                                  _("network '%s' is already defined with uuid %s"),
                                  vm->def->name, uuidstr);
            goto cleanup;
        }

        if (check_active) {
            /* UUID & name match, but if VM is already active, refuse it */
            if (virNetworkObjIsActive(vm)) {
                virNetworkReportError(VIR_ERR_OPERATION_INVALID,
                                      _("network is already active as '%s'"),
                                      vm->def->name);
                goto cleanup;
            }
        }

        dupVM = 1;
    } else {
        /* UUID does not match, but if a name matches, refuse it */
        vm = virNetworkFindByName(doms, def->name);
        if (vm) {
            char uuidstr[VIR_UUID_STRING_BUFLEN];
            virUUIDFormat(vm->def->uuid, uuidstr);
            virNetworkReportError(VIR_ERR_OPERATION_FAILED,
                                  _("network '%s' already exists with uuid %s"),
                                  def->name, uuidstr);
            goto cleanup;
        }
    }

    ret = dupVM;
cleanup:
    if (vm)
        virNetworkObjUnlock(vm);
    return ret;
}


void virNetworkObjLock(virNetworkObjPtr obj)
{
    virMutexLock(&obj->lock);
}

void virNetworkObjUnlock(virNetworkObjPtr obj)
{
    virMutexUnlock(&obj->lock);
}
