# frozen_string_literal: true

cause = RuntimeError.new(+"outer")
inner = RuntimeError.new(+"inner")
begin
  begin
    raise cause
  rescue RuntimeError
    begin
      raise inner
    ensure
      GC.start
      p [$!.equal?(inner), $!.cause.equal?(cause)]
    end
  end
rescue RuntimeError => caught
  p [caught.equal?(inner), caught.cause.equal?(cause)]
end
