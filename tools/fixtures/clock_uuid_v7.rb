require "securerandom"
puts "ns=#{Process.clock_gettime(Process::CLOCK_REALTIME, :nanosecond)}"
puts "us=#{Process.clock_gettime(Process::CLOCK_REALTIME, :microsecond)}"
puts "ms=#{Process.clock_gettime(Process::CLOCK_REALTIME, :millisecond)}"
puts "s=#{Process.clock_gettime(Process::CLOCK_REALTIME, :second)}"
(0..12).each do |bits|
  id = SecureRandom.uuid_v7(extra_timestamp_bits: bits)
  puts "#{bits}: #{id[0, 13]}"
end
