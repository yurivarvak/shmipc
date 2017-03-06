
ifeq ($(PRO_MACHINE_TYPE), x86e_win64)
CC = cl /O2 /Zi
CPP = cl /O2 /EHsc /I. /Zi
OBJEXT = .obj
EXEEXT = .exe
LINKLIBS =
else
CC = gcc -std=gnu99 -O2
CPP = g++ -std=c++11 -I. -O2
OBJEXT = .o
EXEEXT = 
LINKLIBS = -lrt -pthread
endif

all: shmipc$(OBJEXT) basicipc$(OBJEXT) testclient$(EXEEXT) testserver$(EXEEXT)

shmipc$(OBJEXT): shmipc.cxx
	$(CPP) -o $@ -c $<

basicipc$(OBJEXT): basicipc.c
	$(CC) -o $@ -c $<

testclient$(OBJEXT): testclient.c
	$(CC) -o $@ -c $<

testclient$(EXEEXT): testclient$(OBJEXT)
	$(CPP) -o $@ $< shmipc$(OBJEXT) basicipc$(OBJEXT) $(LINKLIBS)

testserver$(OBJEXT): testserver.c
	$(CC) -o $@ -c $<

testserver$(EXEEXT): testserver$(OBJEXT)
	$(CPP) -o $@ $< shmipc$(OBJEXT) basicipc$(OBJEXT) $(LINKLIBS)
