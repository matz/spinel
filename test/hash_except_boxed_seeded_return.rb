# Hash#except on a boxed receiver answers the general hash. A seeded
# Hash[String, String] or Hash[String, Integer] return rebuilds it in the
# return's own variant; returned as it stood, it was another struct.

class Tagged
  def attributes = { "id" => "a1", "class" => "wide", "title" => "One", "alt" => nil }
  def counts = { "n" => 1, "class" => 2, "gone" => nil }
end

class Plain
  def attributes = { "id" => "b2" }
  def counts = { "m" => 3 }
end

class Keyed
  def attributes = { 1 => 2 }
  def counts = { 3 => 4 }
end

class Pick
  #: (untyped) -> Hash[String, String]
  def without_class(el)
    attrs = el.attributes
    attrs.key?("class") ? attrs.except("class") : attrs
  end

  #: (untyped) -> Hash[String, Integer]
  def counts_without_class(el)
    counts = el.counts
    counts.key?("class") ? counts.except("class") : counts
  end
end

els = [Tagged.new, Plain.new, Keyed.new]
pick = Pick.new

kept = pick.without_class(els[0])
p kept
p kept.size
p kept["title"]
p kept.key?("class")
p kept.key?("alt")
p kept["alt"]
kept.each { |name, val| p [name, val] }
p pick.without_class(els[1])

counts = pick.counts_without_class(els[0])
p counts
p counts["n"] + 10
p counts.key?("gone")
p counts["gone"]
p pick.counts_without_class(els[1])

p els[2].attributes
p els[2].counts
