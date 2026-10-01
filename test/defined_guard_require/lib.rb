module GuardLib
  TABLE = { a: 1 }
  def self.get(k) = TABLE[k]
end
puts "lib loaded"
