
O = ../O
BIN = ../O
all: $(O)/libcalc.a

LIBS = libscanner.c libnumber.c libmdcode.c
$(BIN)/parsergen: $(LIBS)
	$(QUIET_CC)$(CC) -o $@ -fplan9-extensions parsergen.c $(LIBS) -licuuc -lgmp

$(O)/calc.c : $(BIN)/parsergen calc.mdc
	$(QUIET_GEN)$(BIN)/parsergen --tag calc -o $(O)/calc calc.mdc

LIBOBJ = $(patsubst %.c,$(O)/%.o,$(LIBS) libparser.c)
$(LIBOBJ) : $(O)/%.o : %.c
	$(QUIET_CC)$(CC) -g -c -o $@ -fPIC -fplan9-extensions $^
$(O)/calc.o : $O/calc.c
	$(QUIET_CC)$(CC) -g -c -I. -o $@ -fPIC -fplan9-extensions $^

$(O)/libcalc.a : $(LIBOBJ) $(O)/calc.o
	$(QUIET_AR)$(AR) cr $@ $^

#
# Pretty print
#
V	      = @
Q	      = $(V:1=)
QUIET_CC      = $(Q:@=@echo    '     CC       '$@;)
QUIET_AR      = $(Q:@=@echo    '     AR       '$@;)
QUIET_GEN     = $(Q:@=@echo    '     GEN      '$@;)
