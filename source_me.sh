export LIBRARY_PATH=`pwd`/lib:$LIBRARY_PATH
export LD_LIBRARY_PATH=`pwd`/lib:$LD_LIBRARY_PATH
export DYLD_LIBRARY_PATH=`pwd`/lib:$DYLD_LIBRARY_PATH
export LD_INCLUDE_PATH=`pwd`/include:$LD_INCLUDE_PATH
# Setting include directories via C_INCLUDE_PATH/CPLUS_INCLUDE_PATH will
# automatically get them demoted to the end of the search list even if a -I
# option is passed to try and bump them up earlier, before other -I options.
# We leave the Makefile in charge of finding all the include directories.
export CFLAGS="-I $(pwd)/include ${CFLAGS}"
export CXXFLAGS="-I $(pwd)/include -I$(pwd)/include/dynamic ${CXXFLAGS}"
export PATH=`pwd`/bin:`pwd`/scripts:$PATH
export CC=$(which gcc)
export CXX=$(which g++)
export CPLUS_INCLUDE_PATH=/broad/software/free/Linux/redhat_7_x86_64/pkgs/cairo_1.14.2/include/cairo/:$CPLUS_INCLUDE_PATH
source /broad/software/scripts/useuse
reuse -q .boost-1.70.0 
reuse -q .bison-3.0.4 
reuse -q .flex-2.5.39
reuse -q .cairo-1.14.2
reuse -q .zlib-1.2.8

#
#  disable until file arguments work as in normal bash :(
#
# add bash autocompletion
#if test -n "$BASH_VERSION"
#then
#
#	 . ./autocomp.bash
#fi
