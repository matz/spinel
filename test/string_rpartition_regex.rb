# spinel: share
# spinel: gc-stress
# Issue #1226: String#rpartition with a regex used to SEGV — rpartition
# had no regex branch, so the pattern compiled to a NULL pointer that
# was passed to strlen/strstr. The regex form must locate the *last*
# match by start position: \d+ in "hello123world" matches at 5/6/7, so
# the rightmost is the single "3" (not "123").
puts "hello123world".rpartition(/\d+/).inspect
puts "foo1bar2baz".rpartition(/\d/).inspect
puts "no digits here".rpartition(/\d+/).inspect
puts "a.b.c".rpartition(".").inspect
puts "abc".rpartition("x").inspect

# The rightmost match owns all backrefs; a miss clears them.
/(stale)/ =~ "stale"
p "abcb".rpartition(/(b)(z)?/)
p [$~.to_a, $1, $2, $&, $`, $', $+]
p "abc".rpartition(/(z)/)
p [$~, $1, $&, $`, $', $+]
p "aaa".rpartition(/(a+)/)
p [$~.to_a, $~.byteoffset(0)]
p "hé".rpartition(/()/)
p [$~.to_a, $~.byteoffset(0)]
# A String separator leaves the previous match alone.
p "a.b".rpartition(".")
p [$~.to_a, $1]
