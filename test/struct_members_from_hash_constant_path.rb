# Struct.new and Data.define read their members from a literal Hash
# constant reached by its namespace (`Lock::DEFAULTS.keys`,
# `Search::Registry::OPTIONS[:delayed]`, `::TOP.keys`) as they read it by
# its bare name, under the same conditions.
module Search
  class Registry
    OPTIONS = { delayed: %i[latency ttl], plain: %i[ttl] }.freeze
  end
end
class Lock
  DEFAULTS = { retries: 3, timeout: 1.5 }.freeze
  Attrs = Struct.new(*DEFAULTS.keys)
end
TOP = { a: 1, b: 2 }

Config = Struct.new(*Lock::DEFAULTS.keys)
p Config.new(*Lock::DEFAULTS.values).to_a
p Config.members
Delayed = Struct.new(*Search::Registry::OPTIONS[:delayed])
p Delayed.members
Plain = Data.define(*Search::Registry::OPTIONS[:plain])
p Plain.new(ttl: 4)
Top = Struct.new(*::TOP.keys)
p Top.new(1, 2).b
p Lock::Attrs.members
