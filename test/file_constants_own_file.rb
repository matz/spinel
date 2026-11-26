# spinel: not-cruby -- CRuby returns the builtin File's 0 for IO::RDONLY here; Spinel leaves IO::X as written beside a File class of the program's own, and the read raises NameError
# An IO::X read beside a File class of the program's own does not return
# that class's constant.
module App
  class File
    RDONLY = 99
  end
  p File::RDONLY
  begin
    p IO::RDONLY
  rescue NameError => e
    puts e.message
  end
end
