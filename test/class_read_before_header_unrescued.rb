# A class read above its header, unrescued, ends the program with
# NameError, as CRuby does. The sidecar holds the error line as Spinel
# prints it.

puts "before"
Later.new
puts "not reached"
class Later; end
