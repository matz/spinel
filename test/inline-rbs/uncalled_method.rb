# A module method nothing calls, followed by another def, keeps its inline
# signature: the compile prints no warning, and an --rbs signature that
# disagrees with it stops the compile and names both.
module Names
  TABLE = { "a" => "b" }.freeze

  #: (String) -> String?
  def self.lookup(key)
    TABLE[key]
  end

  #: (String) -> String?
  def self.key(name)
    TABLE[name]
  end
end
puts "unused"
