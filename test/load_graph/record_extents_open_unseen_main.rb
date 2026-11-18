# A require of a name the program computes may load code the record has not
# read under the inline model, which may read the constant: the lambda then
# starts where its write runs. The graph model reads the file and starts it
# at the call there.
L = -> { p :unseen }
name = "record_extents_open_unseen" + "_lib"
require_relative name
