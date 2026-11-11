# A lookup by a name (instance_method, public_instance_method, method,
# public_method) raises NameError for a name that is nowhere, which a rescue
# of NameError, or a bare rescue, catches. A literal name the program proves
# is nowhere for the subject raises CRuby's NameError with its words; a name
# the program defines only on a class that is not the subject's, nor one of
# its ancestors, is nowhere for the subject; a computed name keeps answering
# as it did.
def name_error
  yield
rescue NameError => e
  puts "#{e.class == NameError ? "NameError" : "a NameError"}: #{e.message}"
end

name_error { Integer.instance_method(:nope) }
name_error { String.public_instance_method(:nope) }
name_error { Comparable.instance_method(:nope) }
name_error { 5.method(:nope) }
p((Integer.instance_method(:nope) rescue :none))
p((String.public_instance_method(:nope) rescue :none))

class Elsewhere; def zonk = 1; end
module Mixed; def zink = 1; end
class Other; include Mixed; end
begin
  Integer.instance_method(:zonk)
rescue NameError => e
  puts "NameError: #{e.message}"
end
begin
  Integer.instance_method(:zink)
rescue NameError => e
  puts "NameError: #{e.message}"
end
p((Integer.instance_method(:zonk) rescue :none))
p((Integer.instance_method(:zink) rescue :none))

name = [:nope].first
p((Integer.instance_method(name) rescue :none))
