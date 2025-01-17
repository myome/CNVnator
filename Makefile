VERSION	       = v0.4.1

override LIBS += -lz -lbz2 -lcurl -llzma -lreadline -lbam -lhts -lCore -lRIO -lHist -lGraf -lGpad -lTree -lMathCore

# TODO(James): Replace this with the typical autoconf method.
INC := -I${INSTALL_PREFIX}/include
LDD_PATH := ${INSTALL_PREFIX}/lib

ifeq ($(OMP),no)
        $(info Compiling with NO parallel support)
else
        OMPFLAGS = -fopenmp
        $(info Compiling with parallel (OpenMP) support)
endif

ifneq ($(YEPPPLIBDIR),)
        override LIBS += -L$(YEPPPLIBDIR) -lyeppp
endif

ifneq ($(YEPPPINCLUDEDIR),)
        INC += -I$(YEPPPINCLUDEDIR) -DUSE_YEPPP
endif

CXX    = g++ -O3 -std=c++11 -DCNVNATOR_VERSION=\"$(VERSION)\" $(OMPFLAGS)
#CXX    = g++ -O3 -D_GLIBCXX_USE_CXX11_ABI=0 -std=c++11 -DCNVNATOR_VERSION=\"$(VERSION)\" $(OMPFLAGS)

OBJDIR = obj
OBJS   = $(OBJDIR)/cnvnator.o \
	 $(OBJDIR)/EXOnator.o \
	 $(OBJDIR)/IO.o \
	 $(OBJDIR)/Visualizer.o \
	 $(OBJDIR)/HisMaker.o \
	 $(OBJDIR)/AliParser.o \
	 $(OBJDIR)/FastaParser.o \
	 $(OBJDIR)/VcfParser.o \
	 $(OBJDIR)/Genotyper.o \
	 $(OBJDIR)/Interval.o \
	 $(OBJDIR)/Genome.o

DISTRIBUTION = $(PWD)/CNVnator_$(VERSION).zip
TMPDIR	     =  /tmp
CNVDIR	     = CNVnator_$(VERSION)
MAINDIR	     = $(TMPDIR)/$(CNVDIR)
SRCDIR	     = $(MAINDIR)/src

all: cnvnator

cnvnator: $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LIBS) -L$(LDD_PATH)

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(OBJDIR)
	$(CXX) $(INC) -c $< -o $@

clean:
	rm -fr $(OBJDIR) cnvnator

distribution: clean all
	@echo Creating directory ...
	@rm -rf $(MAINDIR)
	@rm -f $(DISTRIBUTION)
	@mkdir $(MAINDIR)
	@mkdir $(SRCDIR)
	@echo Copying files ...
	@cp *.hh *.cpp $(SRCDIR)
	@cp Makefile $(SRCDIR)
	@cp README.md $(MAINDIR)
	@cp ReleaseNotes.md $(MAINDIR)
	@cp INSTALL $(MAINDIR)
	@cp CITATION $(MAINDIR)
	@cp license.rtf $(MAINDIR)
	@cp -r ExampleData $(MAINDIR)
	@cp -r pytools cnvnator2VCF.pl plotbaf.py plotrdbaf.py plotcircular.py $(SRCDIR)
	@echo Zipping ...
	@ln -s $(MAINDIR)
	@zip -qr $(DISTRIBUTION) $(CNVDIR)
	@rm $(CNVDIR)
