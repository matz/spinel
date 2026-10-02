# String#scrub! changes the receiver, so another name for it sees the
# scrubbed text, as it does for sub! (#7031).
h = +"x\xffy"; o = h; o << ""
h.scrub!("!")
p o
s = +"a\xffb"; t = s
s.scrub!
p t == s
a = [+"c\xffd"]
a[0].scrub!("?")
p a
clean = +"ok"; alias_of_clean = clean
p clean.scrub!, alias_of_clean
