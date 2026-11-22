# In a program that freezes an IO, a bare puts and p print as before
class Own
  def self.puts(*a) = print("[", *a, "]\n")
  puts "puts"
end

x = 1
x = nil
g = {}
g[x] = 1
g[2] = 3
p g, g.keys
STDERR.freeze
