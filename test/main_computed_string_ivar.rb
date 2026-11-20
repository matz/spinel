# frozen_string_literal: true

def key(name)
  "@" + name
end

@slot = 1
s = +"in the static"
t = +"in the table"
self.instance_variable_set(key("slot"), s)
self.instance_variable_set(key("extra"), t)
GC.start
p [@slot.equal?(s), self.instance_variable_get(key("slot")).equal?(s)]
p self.instance_variable_get(key("extra")).equal?(t)
