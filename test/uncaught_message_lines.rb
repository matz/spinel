# The report of an uncaught exception puts the class after the first line of
# a message of several lines, and the other lines after it, as CRuby does
# (and as detailed_message writes it); a lone final newline is replaced by
# the class, and the other lines keep their bytes.
def boom = raise("first line\nsecond line\n  third, indented\n")
boom
