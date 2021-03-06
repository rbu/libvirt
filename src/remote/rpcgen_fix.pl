# Fix XDR code (generated by rpcgen) so that it compiles
# with warnings turned on.
#
# This code is evil.  Arguably better would be just to compile
# without -Werror.  Update: The IXDR_PUT_LONG replacements are
# actually fixes for 64 bit, so this file is necessary.  Arguably
# so is the type-punning fix.
#
# Copyright (C) 2007 Red Hat, Inc.
#
# See COPYING for the license of this software.
#
# Richard Jones <rjones@redhat.com>

use strict;

my $in_function = 0;
my @function = ();

while (<>) {
    if (m/^{/) {
	$in_function = 1;
	print;
	next;
    }

    s/\t/        /g;

    # Portability for Solaris RPC
    s/u_quad_t/uint64_t/g;
    s/quad_t/int64_t/g;
    s/xdr_u_quad_t/xdr_uint64_t/g;
    s/xdr_quad_t/xdr_int64_t/g;
    s/(?<!IXDR_GET_INT32 )IXDR_GET_LONG/IXDR_GET_INT32/g;
    s,#include "\./remote/remote_protocol\.h",#include "remote_protocol.h",;

    if (m/^}/) {
	$in_function = 0;

	# Note: The body of the function is in @function.

	# Remove decl of buf, if buf isn't used in the function.
	my @uses = grep /\bbuf\b/, @function;
	@function = grep !/\bbuf\b/, @function if @uses == 1;

	# Remove decl of i, if i isn't used in the function.
	@uses = grep /\bi\b/, @function;
	@function = grep !/\bi\b/, @function if @uses == 1;

	# (char **)&objp->... gives:
	# warning: dereferencing type-punned pointer will break
	#   strict-aliasing rules
	# so rewrite it.
	my %uses = ();
	my $i = 0;
	foreach (@function) {
	    $uses{$1} = $i++ if m/\(char \*\*\)\&(objp->[a-z_.]+_val)/i;
	}
	if (keys %uses >= 1) {
	    my $i = 1;

	    foreach (keys %uses) {
		$i = $uses{$_};
		unshift @function,
		("        char **objp_cpp$i = (char **) (void *) &$_;\n");
		$i++;
	    }
	    @function =
		map { s{\(char \*\*\)\&(objp->[a-z_.]+_val)}
		       {objp_cpp$uses{$1}}gi; $_ } @function;
	}

	# The code uses 'IXDR_PUT_{U_,}LONG' but it's wrong in two
	# ways: Firstly these functions are deprecated and don't
	# work on 64 bit platforms.  Secondly the return value should
	# be ignored.  Correct both these mistakes.
	@function =
	    map { s/\bIXDR_PUT_((U_)?)LONG\b/(void)IXDR_PUT_$1INT32/; $_ }
	    map { s/\bXDR_INLINE\b/(int32_t*)XDR_INLINE/; $_ }
	    @function;

	print (join ("", @function));
	@function = ();
    }

    unless ($in_function) {
	print;
    } else {
	push @function, $_;
    }
}
