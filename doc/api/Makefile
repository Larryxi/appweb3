#
#	Makefile -- API Documentation makefile
#
#	Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.
#

include		.makedep

gen doc: prep mpr.html ejs.html appweb.html

prep:
	rm -f prep mpr.html ejs.html appweb.html

mpr.html: Makefile mprBare.html *.tem
	rm -f mpr.html
	cat mprHeader.tem >mpr.html
	cat mprBare.html apiFooter.tem >>mpr.html

ejs.html: Makefile ejsBare.html *.tem
	rm -f ejs.html
	cat ejsHeader.tem >ejs.html
	cat ejsBare.html apiFooter.tem >>ejs.html

appweb.html: Makefile appwebBare.html *.tem
	make -C appweb doc
	rm -f appweb.html
	cat appwebHeader.tem >appweb.html
	cat appwebBare.html apiFooter.tem >>appweb.html

tidy:
	tidy -q -config tidy.conf *.html

dependExtra:
	chmod +w *.html

#
#   Local variables:
#   tab-width: 4
#   c-basic-offset: 4
#   End:
#   vim: sw=4 ts=4 noexpandtab
#
