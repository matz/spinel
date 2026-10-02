s = +"a\xff"
s.scrub! { |bad| "?" }
p s
