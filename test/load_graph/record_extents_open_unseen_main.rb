# A require of a name the program computes: the load graph reads the file
# the name gives, whose code may read the constant, so the lambda starts at
# the call there, not where its write runs.

L = -> { p :unseen }
name = "record_extents_open_unseen" + "_lib"
require_relative name
