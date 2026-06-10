# Only tweetnacl.c is part of the firmware. test_verify.c has its own
# main() and is for the PC round-trip test (see README).
idf_component_register(SRCS "tweetnacl.c"
                       INCLUDE_DIRS ".")
