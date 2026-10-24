# A require_relative written without parentheses that applies a binary
# operator to its literal is refused at compile time (#9898), as it was
# when the line scanner took the literal for the whole path.
require_relative "require_relative_tree/b_value" + ""
puts "m"
