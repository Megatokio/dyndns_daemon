#############################################################################
# 	Makefile for building zasm
#############################################################################

TARGET      = dyndns_daemon

MACHINE     = -m64     # -m32 or -m64 or empty

CC          = gcc
CXX         = g++
DEFINES     = -DNDEBUG -DRELEASE
CFLAGS      = $(MACHINE) -pipe -Os -Wall $(DEFINES)
CXXFLAGS    = $(CFLAGS) -std=c++11
INCPATH     = -I. -ISource -ILibraries
LINK        = g++
LFLAGS      = $(MACHINE)
LIBS        = -lcurl -lpthread
STRIP       = strip

OBJECTS     = \
	main.o \
	FD.o \
	kio.o \
	exceptions.o \
	cstrings.o \
	tempmem.o \
	files.o \
	log.o


.PHONY:	all clean

all: 	$(TARGET)

clean:
		rm *.o $(TARGET)

$(TARGET):  Makefile $(OBJECTS)
	$(LINK) $(LFLAGS) -o $(TARGET) $(OBJECTS) $(LIBS)
	$(STRIP) $(TARGET)


####### Compile

%.o : Source/%.cpp
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -o $@ $<

cstrings.o:
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -o cstrings.o Libraries/cstrings/cstrings.cpp

exceptions.o:
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -o exceptions.o Libraries/kio/exceptions.cpp

FD.o:
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -o FD.o Libraries/unix/FD.cpp

tempmem.o:
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -o tempmem.o Libraries/cstrings/tempmem.cpp

files.o:
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -o files.o Libraries/unix/files.cpp

kio.o:
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -o kio.o Libraries/kio/kio.cpp

log.o:
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -o log.o Libraries/unix/log.cpp



