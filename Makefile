REXEC = R
RCOMMAND = CMD
RBUILD = build
RINSTALL = INSTALL
RCHECK = check
RPDF = Rd2dvi
TARGET = OpenMx_0.1-1.tar.gz
DOCFILE = $(RBUILD)/OpenMx.pdf

# subdirectories
RSOURCE = R
RDOCUMENTS = man
RDATA = data

# file types
RDFILES = *.Rd
RFILES = *.R


nothing:
	@echo \
	'Please type make [build | install | doc | check | clean | veryclean]'

build: build/$(TARGET)

build/$(TARGET): $(RSOURCE)/$(RFILES) $(RDOCUMENTS)/$(RDFILES)
	cd $(RBUILD); $(REXEC) $(RCOMMAND) $(RBUILD) ..

doc:
	rm -rf $(DOCFILE); $(REXEC) $(RCOMMAND) $(RPDF) --pdf --title="OpenMx Reference Manual" --output=$(DOCFILE) $(RDOCUMENTS)/$(RDFILES)

$(RSOURCE)/$(RFILES):

$(RDOCUMENTS)/$(RDFILES):

install: build
	cd $(RBUILD); $(REXEC) $(RCOMMAND) $(RINSTALL) $(TARGET)

check: build
	cd $(RBUILD); $(REXEC) $(RCOMMAND) $(RCHECK) $(TARGET)

clean:
	rm -rf $(RBUILD)/*

veryclean: clean
	find . -name "*~" -exec rm -rf '{}' \;
