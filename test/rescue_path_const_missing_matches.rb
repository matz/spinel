# A rescue clause naming a path under a module that defines const_missing
# keeps matching by the class the hook answers: CRuby calls the hook when an
# exception reaches the clause, where a module without one raises NameError.
class Missing < StandardError; end
class Other < StandardError; end
module Compat
  def self.const_missing(name)
    Object.const_get(name)
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

begin
  begin
    raise Missing, "m"
  rescue Compat::Missing => e
    puts "rescued #{e.class}: #{e.message}"
  end
rescue NameError => e
  puts "NameError: #{e.message}"
end
