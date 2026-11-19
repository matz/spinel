# frozen_string_literal: true
# spinel: gc-stress

def payload_key
  "@" + "payload"
end

self.instance_variable_set(payload_key, +"kept")
GC.start
p self.instance_variable_get(payload_key)
