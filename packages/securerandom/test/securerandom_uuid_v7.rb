# spinel: int64
# Fixed clock and entropy exercise the actual formatter in both runtimes.
require "securerandom"

$v7_ns = 1_700_000_000_123_456_789
$v7_entropy = "\xff\xe1\xd2\xc3\xb4\xa5\x96\x87\x78\x69"
$v7_calls = +""
$v7_wide_ms = false

module Process
  def self.clock_gettime(clock, unit = :float_second)
    raise "wrong clock" unless clock == Process::CLOCK_REALTIME
    $v7_calls << "clock:#{unit};"
    return 0x1_0123_4567_89ab if $v7_wide_ms && unit == :millisecond
    unit == :millisecond ? $v7_ns.div(1_000_000) : $v7_ns
  end
end

module SecureRandom
  def self.random_bytes(n = 16)
    $v7_calls << "bytes:#{n};"
    $v7_entropy.byteslice(0, n).dup
  end
end

(0..12).each do |bits|
  $v7_calls = +""
  uuid = SecureRandom.uuid_v7(extra_timestamp_bits: bits)
  puts "#{bits}: #{uuid} #{$v7_calls}"
  puts uuid.match?(/\A[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\z/)
  ms = uuid[0, 8].to_i(16) * 65_536 + uuid[9, 4].to_i(16)
  puts ms == $v7_ns.div(1_000_000)
  extra = (uuid[15, 3].to_i(16) >> (12 - bits))
  puts extra == $v7_ns.modulo(1_000_000) * (1 << bits) / 1_000_000
end

# Same timestamps may produce identical UUIDs, or decreasing random suffixes.
[0, 6, 12].each do |bits|
  $v7_entropy = "\xff" * 10
  first = SecureRandom.uuid_v7(extra_timestamp_bits: bits)
  puts first == SecureRandom.uuid_v7(extra_timestamp_bits: bits)
  $v7_entropy = "\x00" * 10
  second = SecureRandom.uuid_v7(extra_timestamp_bits: bits)
  puts "same #{bits}: #{first} #{second}"
  puts first > second
  $v7_ns -= 1_000_000
  older = SecureRandom.uuid_v7(extra_timestamp_bits: bits)
  puts "rollback #{bits}: #{older}"
  puts older < second
end

# Exercise sub-millisecond rounding at the ends and one 12-bit boundary.
[0, 1, 244, 245, 999_999].each do |ns|
  $v7_ns = 1_700_000_000_123_000_000 + ns
  (0..12).each do |bits|
    uuid = SecureRandom.uuid_v7(extra_timestamp_bits: bits)
    puts "#{ns}/#{bits}: #{uuid}"
  end
end

# Negative time uses floor division; timestamp storage keeps the low 48 bits.
$v7_ns = -1
[0, 1, 12].each { |bits| puts SecureRandom.uuid_v7(extra_timestamp_bits: bits) }

# Realtime milliseconds are truncated to the low 48 bits.
$v7_wide_ms = true
puts SecureRandom.uuid_v7
