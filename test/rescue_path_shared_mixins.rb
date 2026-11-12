# A rescue clause naming a path under a module looks the name up in the
# module and everything it includes. Modules that each include several of
# the ones before them are reached by a number of routes that grows with
# every link; the lookup asks each module once, so this program compiles in
# the time a chain of single includes does.
module M0
  class Held < StandardError; end
end
module M1; include M0; end
module M2; include M1; include M0; end
module M3; include M2; include M1; include M0; end
module M4; include M3; include M2; include M1; end
module M5; include M4; include M3; include M2; end
module M6; include M5; include M4; include M3; end
module M7; include M6; include M5; include M4; end
module M8; include M7; include M6; include M5; end
module M9; include M8; include M7; include M6; end
module M10; include M9; include M8; include M7; end
module M11; include M10; include M9; include M8; end
module M12; include M11; include M10; include M9; end
module M13; include M12; include M11; include M10; end
module M14; include M13; include M12; include M11; end
module M15; include M14; include M13; include M12; end
module M16; include M15; include M14; include M13; end
module M17; include M16; include M15; include M14; end
module M18; include M17; include M16; include M15; end
module M19; include M18; include M17; include M16; end
module M20; include M19; include M18; include M17; end
module M21; include M20; include M19; include M18; end
module M22; include M21; include M20; include M19; end
module M23; include M22; include M21; include M20; end
module M24; include M23; include M22; include M21; end
module M25; include M24; include M23; include M22; end
module M26; include M25; include M24; include M23; end
module M27; include M26; include M25; include M24; end
module M28; include M27; include M26; include M25; end
module M29; include M28; include M27; include M26; end
module M30; include M29; include M28; include M27; end
module M31; include M30; include M29; include M28; end
module M32; include M31; include M30; include M29; end
module M33; include M32; include M31; include M30; end
module M34; include M33; include M32; include M31; end

def held
  raise M0::Held, "h"
rescue M34::Held => e
  "rescued #{e.class}: #{e.message}"
end
puts held

begin
  begin
    raise "x"
  rescue M34::Nope
    puts "rescued"
  end
rescue NameError => e
  puts "#{e.class}: #{e.message} (#{e.cause.class})"
end
