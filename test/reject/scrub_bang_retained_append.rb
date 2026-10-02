s = +"a\xff"
r = s.scrub!
r << "Z"
p s, r
