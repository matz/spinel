# frozen_string_literal: true

$events = []
def receiver(h)
  $events << :receiver
  h
end

def value(key)
  $events << :value
  key << "!"
  GC.start
  "saved"
end

s = +"key"
h = { "seed" => "value" }
receiver(h)[[s].map { |v| $events << :key; v }.first] = value(s)
p h, $events
