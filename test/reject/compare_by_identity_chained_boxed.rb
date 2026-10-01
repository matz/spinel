# compare_by_identity on a boxed Hash, read out of an Array (see
# compare_by_identity_chained.rb).
h = { a: 1 }
p [h][0].compare_by_identity.size
