OutDir                 :=./bin
OutputFile             :=$(OutDir)/chan-alsa-input.so

##
## Common variables
##
CC       := gcc
CFLAGS   :=  -g -Wall -Wextra -fPIC -pthread -I.
LD       := gcc
LDFLAGS  := -shared -fPIC -pthread -L. -lasound


.PHONY: all clean set_ast_version_18 set_ast_version_110 set_ast_version_130

all:
	echo You must choose one of the following target : for_ast_1.8, for_ast_11 or for_ast_13

for_ast_1.8: set_ast_version_18 $(OutputFile)

for_ast_11: set_ast_version_110 $(OutputFile)

for_ast_13: set_ast_version_130 $(OutputFile)

set_ast_version_18:
	$(eval AST_VERSION := 18)

set_ast_version_110:
	$(eval AST_VERSION := 110)

set_ast_version_130:
	$(eval AST_VERSION := 130)

$(OutDir):
	test -d $(OutDir) || mkdir -p $(OutDir)

$(OutputFile): $(OutDir) $(OutDir)/chan_alsa_input.o
	$(LD) $(LDFLAGS) -o $(OutputFile) $(OutDir)/chan_alsa_input.o $(LibPath) $(Libs)

$(OutDir)/chan_alsa_input.o: $(OutDir) chan_alsa_input.c
	$(CC) -c chan_alsa_input.c $(CFLAGS) $(IncludePath) -DAST_VERSION=$(AST_VERSION) -o $(OutDir)/chan_alsa_input.o

##
## Clean
##
clean:
	$(RM) -r $(IntermediateDirectory)


