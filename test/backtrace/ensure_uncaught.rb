def boom
  raise "boom"
end

begin
  boom
ensure
  if ARGV[0] == "reraise"
    begin
      raise TypeError, "cleanup"
    rescue TypeError
      nil
    end
    raise
  end
end
