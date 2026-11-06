# A method the program defines by a native declaration's name is the
# program's code: a call of it before a constant's assignment may read the
# constant, which raises NameError.
class Holder
  def self.k; K; end
end
def native_lib(name)
  Holder.k
rescue NameError => e
  puts "#{name}: #{e.message}"
end
native_lib("early")
class Holder
  K = 3
end
p Holder.k
