# A const_missing a module is given by define_singleton_method, or by a
# define_method in its `class << self`, is the hook a def of that name is: a
# rescue clause naming a path under the module keeps matching by the class
# the hook answers, where a module without one raises NameError.
class Missing < StandardError; end
class Other < StandardError; end
module Compat; end
Compat.define_singleton_method(:const_missing) { |name| Object.const_get(name) }
module Shim
  class << self
    define_method(:const_missing) { |name| Object.const_get(name) }
  end
end

def fallback(k)
  raise k, "m"
rescue Compat::Missing => e
  "rescued #{e.class}"
rescue StandardError => e
  "next clause #{e.class}"
end
puts fallback(Missing)
puts fallback(Other)

def shim(k)
  raise k, "m"
rescue Shim::Missing => e
  "rescued #{e.class}"
rescue StandardError => e
  "next clause #{e.class}"
end
puts shim(Missing)
puts shim(Other)

begin
  begin
    raise Missing, "m"
  rescue Compat::Missing => e
    puts "rescued #{e.class}: #{e.message}"
  end
rescue NameError => e
  puts "NameError: #{e.message}"
end
