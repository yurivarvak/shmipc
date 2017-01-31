
ifeq ($(PRO_MACHINE_TYPE), x86e_win64)
CC = cl
CPP = cl /EHsc /I.
OBJEXT = .obj
EXEEXT = .exe
else
CC = gcc
CPP = gcc -std=c++11 -I.
OBJEXT = .o
EXEEXT = 
endif

all: shmipc$(OBJEXT) basicipc$(OBJEXT)

shmipc$(OBJEXT): shmipc.cxx
	$(CPP) -o $@ -c $<

basicipc$(OBJEXT): basicipc.c
	$(CC) -o $@ -c $<
