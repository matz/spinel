# Issue #85 / PR #92: an empty `[]` followed by `push(:sym)` should
# promote the local's tracked type to sym_array, so element access
# emits the symbol-name path instead of printing the symbol id.
# The literal-array form (`syms = [:tag, :more]`) was already passing
# on master before PR #92, so this test exercises empty-then-push.

syms = []
syms.push(:tag)
syms.push(:more)
puts syms[0]      # tag
puts syms[1]      # more
puts syms.length  # 2

# `<<` form should behave the same.
syms2 = []
syms2 << :alpha
syms2 << :beta
puts syms2[0]     # alpha
puts syms2[1]     # beta
