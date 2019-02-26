set -x -e

# STEP 1: setup autoconf with stack's sandboxed GHC

stack exec -- ./boot
stack exec -- ./configure


# STEP 2: generate lamdachine's special files

c++ utils/genopcodes.cc -I vm -o genopcodes

./genopcodes > compiler/Opcodes.h

PWD=`pwd`
echo "#define PACKAGE_CONF_STRING \"${PWD}/libraries/package.conf\"" > lcc/Locations.h
