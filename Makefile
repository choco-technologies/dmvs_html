# #############################################################################
# 
# 	dmvs_html - the HTML and CSS converter of dmvsi (a library module).
#
# #############################################################################
DMOD_DIR=@DMOD_DIR@

# -----------------------------------------------------------------------------
#  Paths initialization
# -----------------------------------------------------------------------------
include $(DMOD_DIR)/paths.mk

# -----------------------------------------------------------------------------
#   Module configuration
# -----------------------------------------------------------------------------

# The name of the module
DMOD_MODULE_NAME=dmvs_html

# The version of the module
DMOD_MODULE_VERSION=0.1

# The name of the author
DMOD_AUTHOR_NAME=Patryk Kubiak

# The list of C sources
DMOD_CSOURCES=src/dmvs_html.c src/html.c src/css.c src/tailwind.c src/style.c src/layout.c src/paint.c src/svg.c src/script.c src/build.c src/anim.c

# The list of C++ sources
DMOD_CXXSOURCES=

# The list of include directories
DMOD_INC_DIRS=src

# The list of libraries to link
DMOD_LIBS=

# The list of definitions
DMOD_DEFINITIONS=

# -----------------------------------------------------------------------------
#   List of MAL interfaces implemented by the module
# -----------------------------------------------------------------------------
DMOD_MAL_IMPLS=

# -----------------------------------------------------------------------------
#   Include the dmod app makefile
# -----------------------------------------------------------------------------
include $(DMOD_DMF_LIB_FILE_PATH)
