def with_connection
  yield
end

def deferred
  with_connection do
    begin
      yield
    ensure
      nil
    end
  end
end

p(deferred { 42 })
