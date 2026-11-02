# A read of a class variable no class declares, stored into a slot of
# another kind (a bool class variable, an Integer instance variable, a typed
# local, an Array of Integers): the store compiles as the read's NameError
# raise with the slot's own zero behind it, and the raise runs first.
class T
  @@ready = true
  @@count = 0
  def initialize = (@n = 1)
  def self.go
    @@ready = @@missing
  rescue NameError => e
    puts "go: #{e.message}"
  end
  def set_n
    @n = @@missing
  rescue NameError => e
    puts "set_n: #{e.message} (#{@n})"
  end
  def local
    r = 5
    r = @@missing
    r
  rescue NameError => e
    puts "local: #{e.message}"
    -1
  end
  def push
    a = [1, 2]
    a << @@missing
    a
  rescue NameError => e
    puts "push: #{e.message}"
    []
  end
  def self.count = (@@count += 1)
  def self.ready = @@ready
end
T.go
p T.ready
T.new.set_n
p T.new.local
p T.new.push
p T.count
