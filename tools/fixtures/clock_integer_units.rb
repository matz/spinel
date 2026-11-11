require "securerandom"
puts "ms=#{Process.clock_gettime(Process::CLOCK_REALTIME, :millisecond)}"
puts "us=#{Process.clock_gettime(Process::CLOCK_REALTIME, :microsecond)}"
puts "uuid=#{SecureRandom.uuid_v7[0, 13]}"
