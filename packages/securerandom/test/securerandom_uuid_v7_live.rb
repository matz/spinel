# spinel: int64
require "securerandom"

# No stubs: bracket every entropy draw with real realtime clock reads.
[0, 1, 6, 11, 12].each do |bits|
  5.times do
    before = Process.clock_gettime(Process::CLOCK_REALTIME, :nanosecond)
    uuid = bits == 0 ? SecureRandom.uuid_v7 : SecureRandom.uuid_v7(extra_timestamp_bits: bits)
    after = Process.clock_gettime(Process::CLOCK_REALTIME, :nanosecond)
    puts uuid.match?(/\A[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\z/)
    ms = uuid[0, 8].to_i(16) * 65_536 + uuid[9, 4].to_i(16)
    extra = uuid[15, 3].to_i(16) >> (12 - bits)
    encoded = ms * (1 << bits) + extra
    lo = before.div(1_000_000) * (1 << bits) + before.modulo(1_000_000) * (1 << bits) / 1_000_000
    hi = after.div(1_000_000) * (1 << bits) + after.modulo(1_000_000) * (1 << bits) / 1_000_000
    puts lo <= encoded && encoded <= hi
  end
end

# Concurrent UUID generation shares no formatter state.
[0, 12].each do |bits|
  ids = []
  mutex = Mutex.new
  threads = []
  4.times do
    threads << Thread.new do
      mine = []
      25.times { mine << SecureRandom.uuid_v7(extra_timestamp_bits: bits) }
      mutex.synchronize { ids.concat(mine) }
    end
  end
  threads.each(&:join)
  puts ids.length
  puts ids.uniq.length
end
