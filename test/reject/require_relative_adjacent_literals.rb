# Adjacent literals (`"a" "b"`) are one String to CRuby, but Prism gives
# them as two parts; a require_relative of them is refused (#9898), as it
# was when the line scanner took the first literal for the whole path.
require_relative "require_relative_tree/b_" "value"
puts B_VALUE
