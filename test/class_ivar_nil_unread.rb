# A class or module body's ivar written only nil, and never read, has no
# slot: the write stores nothing, and a value with effects still runs.

def announce = (puts "announced"; nil)

class Store
  @logger = nil
  @logger_set = true
  @hook = announce

  def self.ready? = @logger_set
end

module Registry
  @default = nil
end

class Plain
  @a = @b = nil
end

p Store.ready?
p Store.instance_variable_get(:@logger_set)
p Registry.class
p Plain.name
