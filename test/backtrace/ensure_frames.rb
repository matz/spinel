# Driven by make backtrace-test under --debug --no-inline-hot.
class EnsureFrames
  def leaf
    raise ArgumentError, "original"
  end

  def cleanup
    raise TypeError, "cleanup"
  end

  def run(mode)
    leaf
  ensure
    if mode == 1
      begin
        cleanup
      rescue TypeError
        nil
      end
    elsif mode == 2
      begin
        begin
          cleanup
        ensure
          nil
        end
      rescue TypeError
        nil
      end
    elsif mode == 4
      begin
        cleanup
      rescue TypeError
        nil
      end
      raise
    elsif mode == 3
      cleanup
    end
  end

  def nested
    begin
      run(1)
    ensure
      nil
    end
  ensure
    nil
  end

  def unmatched
    leaf
  rescue TypeError
    nil
  ensure
    nil
  end

  def reraised
    leaf
  rescue ArgumentError
    raise
  ensure
    begin
      cleanup
    rescue TypeError
      nil
    end
  end
end

runner = EnsureFrames.new
[0, 1, 2, 3, 4].each do |mode|
  begin
    runner.run(mode)
  rescue => e
    expected = mode == 3 ? "cleanup" : "leaf"
    raise "wrong first frame: #{e.backtrace.inspect}" unless e.backtrace.first.include?(expected)
    puts "run #{mode}: #{e.message}"
  end
end

begin
  runner.nested
rescue => e
  raise "nested lost leaf" unless e.backtrace.first.include?("leaf")
  puts "nested: #{e.message}"
end

begin
  runner.unmatched
rescue => e
  raise "unmatched rescue lost leaf" unless e.backtrace.first.include?("leaf")
  puts "unmatched: #{e.message}"
end

begin
  runner.reraised
rescue => e
  raise "reraise lost leaf" unless e.backtrace.first.include?("leaf")
  puts "reraised: #{e.message}"
end
