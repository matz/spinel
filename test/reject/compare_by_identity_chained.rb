# Hash#compare_by_identity is refused at compile time (#2086,
# docs/limitations.md), and a call chained onto it must not hide that.
# Untyped, the chained call was spelled as a NoMethodError "for unknown"
# that never emitted its receiver, so the refusal never ran: the program
# built and failed at run time. The variants (compare_by_identity_chained_*)
# are one shape each, since a refusal is reported once.
h = { a: 1 }
p h.compare_by_identity.size
