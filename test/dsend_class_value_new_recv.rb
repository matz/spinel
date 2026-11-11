# A send with a computed name whose receiver is `klass.new`, with klass a
# class value only the run time knows (Object.const_get), and whose answer
# is used. The receiver was not taken into a local the arms read, so each
# arm kept its own `klass.new`; the String mutator arms (append_as_bytes,
# capitalize!, ...) then took the receiver's String handle ahead of the
# dispatch, whatever the name: type inference did not converge, and the
# send raised "undefined method 'append_as_bytes' for an instance of Base".
class Base
  def index = "index"
  def show = "show"
end

class Other
  def index = "other"
end

class Counter
  @@made = 0
  def self.made = @@made
  def initialize = @@made += 1
  def index = "counter #{@@made}"
end

class Missing
  H = { "a" => "x" }
  def self.new = H["zz"]
end

class Made
  def self.new = +"made"
end

klass = Object.const_get(ARGV[0] || "Base")
puts klass.new.public_send("index".to_sym)

answer = klass.new.public_send(("sh" + "ow").to_sym)
p answer

pick = [Base, Other][ARGV.size + 1]
puts pick.new.send("index".to_sym)

def build(klass) = klass.new.public_send("index".to_sym)
puts build(Object.const_get(ARGV[1] || "Counter"))
p Counter.made

%w[Missing Made].each do |name|
  k = Object.const_get(name)
  p k.new.public_send("to_s".to_sym)
  p k.new.public_send(("up" + "case").to_sym)
rescue NoMethodError => e
  p e.class
end

# A class value whose builtin arm is String: its mutator arm mutates the
# String `new` made, and Words' own upcase! answers for a Words.
class Words
  def initialize(*) = nil
  def upcase! = "words!"
end
pair = [Words, String][ARGV.size + 1]
p pair.new("ab").public_send(("up" + "case!").to_sym)
p [String, Words][ARGV.size + 1].new("ab").public_send(("up" + "case!").to_sym)

# A user's own instance method `new` answering a typed Hash's miss: the
# local holds a nil, and the arms still answer for nil.
class Table
  H = { "a" => +"x" }
  def new = H[ARGV[2] || "zz"]
end
table = Table.new
p table.new.public_send(("ni" + "l?").to_sym)
p table.new.public_send(("to" + "_s").to_sym)
begin
  p table.new.public_send(("up" + "case!").to_sym)
rescue NoMethodError => e
  puts e.message
end
