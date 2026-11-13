# Struct.new and Data.define take their members from a literal Hash
# constant: its symbol keys (`*DEFAULTS.keys`), or the symbol list one of
# its symbol keys holds (`*OPTIONS[:delayed]`), frozen or not, as long as
# every read of the constant leaves it as written.
OPTIONS = { delayed: %i[latency ttl], plain: %i[ttl] }.freeze
DEFAULTS = { retries: 3, timeout: 1.5 }.freeze
OPEN = { host: "h", port: 1 }

Config = Struct.new(*DEFAULTS.keys)
p Config.new(*DEFAULTS.values).to_a
p Config.members
Delayed = Struct.new(*OPTIONS[:delayed])
p Delayed.members
p Delayed.new(1, 2).ttl
Plain = Data.define(*OPTIONS[:plain])
p Plain.new(ttl: 5)
Conn = Struct.new(*OPEN.keys, keyword_init: true)
c = Conn.new(**OPEN)
p [c.host, c.port]
p OPEN.size, OPEN.key?(:host), DEFAULTS[:retries]
