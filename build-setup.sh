set -x -e

c++ utils/genopcodes.cc -I vm -o genopcodes

./genopcodes > compiler/Opcodes.h

echo "#define PACKAGE_CONF_STRING \"$(PWD)/libraries/package.conf\"" > lcc/Locations.h
