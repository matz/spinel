# A reject!, select!, delete_if or keep_if over a String Array a flat_map made:
# the Array holds its Strings as handles and the block's parameter, left
# widened by the scope, takes the handle too, so the block's test reads the
# element and a bang in the block changes it.
files = ["a", "b"].flat_map { |d| [d] }
files.reject! { |f| f == "b" }
p files
g = ["x", "yy", "zzz"].flat_map { |d| [d, d + "!"] }
g.select! { |f| f.size > 2 }
p g
h = %w[k l m].flat_map { |d| [d] }
h.delete_if { |f| f == "l" }
p h
h.keep_if { |f| f != "m" }
p h
i = ["a", "b", "c"].flat_map { |d| [d * 2] }
p i.reject! { |f| f == "zz" }
p i.select! { |f| f.start_with?("a", "b") }
p i
j = ["q"].flat_map { |d| [d + "r"] }
j.reject! { |f| f << "!"; false }
p j
