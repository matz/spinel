# frozen_string_literal: true

class Date
  # Bitmask flag constants for strftime parsing
  FL_LEFT   = 0x01  # '-' flag
  FL_SPACE  = 0x02  # '_' flag
  FL_ZERO   = 0x04  # '0' flag
  FL_UPPER  = 0x08  # '^' flag
  FL_CHCASE = 0x10  # '#' flag
  private_constant :FL_LEFT, :FL_SPACE, :FL_ZERO, :FL_UPPER, :FL_CHCASE

  # call-seq:
  #   strftime(format = '%F') -> string
  #
  # Returns a string representation of the date in +self+,
  # formatted according the given +format+:
  #
  #   Date.new(2001, 2, 3).strftime # => "2001-02-03"
  #
  # For other formats, see
  # {Formats for Dates and Times}[rdoc-ref:language/strftime_formatting.rdoc].
  def strftime(format = DEFAULT_STRFTIME_FMT)
    if format.equal?(DEFAULT_STRFTIME_FMT)
      internal_civil unless @year
      result = fast_ymd.force_encoding(Encoding::US_ASCII)
      fmt = DEFAULT_STRFTIME_FMT
    else
      fmt = Date.__send__(:to_string_value, format)
      if fmt == YMD_FMT
        internal_civil unless @year
        result = fast_ymd.force_encoding(Encoding::US_ASCII)
      else
        result = internal_strftime(fmt).force_encoding(fmt.encoding)
      end
    end
    # The C extension formats into an output buffer that grows 1024, 2048, ...
    # and gives up with Errno::ERANGE once the buffer would reach 1024 * flen
    # (flen = format string length) while still not fitting. The effective
    # limit is thus the smallest 1024 * 2**k that is >= 1024 * flen, i.e.
    # 1024 << ceil(log2(flen)); output whose byte length reaches it raises.
    # The limit is always >= 1024, so skip the check entirely for shorter output.
    if result.bytesize >= 1024
      raise Errno::ERANGE, "strftime" if result.bytesize >= 1024 << (fmt.bytesize - 1).bit_length
    end
    result
  end

  private

  def internal_strftime(fmt)
    # Fast path for common format strings (whole-string match)
    case fmt
    when '%Y-%m-%d', '%F'
      internal_civil unless @year
      return fast_ymd
    when '%H:%M:%S', '%T', '%X'
      # +: this becomes the result string, which #strftime force_encodes in
      # place, so it must be mutable (strftime_hms may be a frozen literal).
      return +strftime_hms
    when '%m/%d/%y', '%D', '%x'
      internal_civil unless @year
      return "#{PAD2[@month]}/#{PAD2[@day]}/#{PAD2[@year % 100]}"
    when '%Y-%m-%dT%H:%M:%S%z'
      internal_civil unless @year
      return "#{fast_ymd}T#{strftime_hms}#{strftime_zoff}"
    when '%a %b %e %H:%M:%S %Y', '%c'
      internal_civil unless @year
      return fmt_asctime_str
    when '%A, %B %d, %Y'
      internal_civil unless @year
      w = (@jd + 1) % 7
      y = @year
      y_s = y >= 1000 ? y.to_s : (y >= 0 ? format('%04d', y) : format('-%04d', -y))
      return "#{DAY_FULL_COMMA[w]}#{MONTH_FULL_SPACE[@month]}#{PAD2[@day]}, #{y_s}"
    end

    result = +''.encode(fmt.encoding)
    sc = StringScanner.new(fmt.dup)

    until sc.eos?
      # Batch collect literal (non-%) characters
      if (lit = sc.scan(/[^%]+/))
        result << lit
        next
      end

      # Must be '%' or something unexpected
      unless sc.skip(/%/)
        result << sc.getch
        next
      end

      if sc.eos?
        result << '%'
        break
      end

      # Quick dispatch: if next char is a simple letter spec (A-Z excluding E/O,
      # or a-z), skip flag/width parsing entirely.
      if (quick = sc.scan(/[A-DFG-NP-Za-z]/))
        fast = fast_spec(quick.ord)
        if fast
          result << fast
          next
        end
        sc.unscan
      end

      # Record the position just after '%' so the original text can be
      # restored verbatim if the spec turns out to be invalid.
      spec_start = sc.charpos

      # Parse flags (bitmask)
      flags = 0
      while (f = sc.scan(/[-_0^#]/))
        case f
        when '-' then flags |= FL_LEFT
        # '_' (space) and '0' (zero) padding are mutually exclusive; whichever
        # is specified last wins, matching the C extension.
        when '_' then flags = (flags | FL_SPACE) & ~FL_ZERO
        when '0' then flags = (flags | FL_ZERO) & ~FL_SPACE
        when '^' then flags |= FL_UPPER
        when '#' then flags |= FL_CHCASE
        end
      end

      # Parse width
      width = sc.scan(/\d+/)&.to_i
      raise Errno::ERANGE, "strftime" if width && width > STRFTIME_MAX_WIDTH

      # Flags must precede the width. A flag appearing after the width makes
      # the spec invalid, so emit what was scanned so far verbatim (as the C
      # extension does) and resume scanning from the stray flag.
      if sc.check(/[-_0^#]/)
        result << '%' << fmt[spec_start...sc.charpos]
        next
      end

      # The C extension's strftime grammar has no precision field, so a '.'
      # here makes the spec invalid. Emit the text up to and including it
      # verbatim (as the C extension does) and resume scanning after it.
      if sc.skip(/\./)
        result << '%' << fmt[spec_start...sc.charpos]
        next
      end

      # Colons follow the flags and width, and are only meaningful for %z.
      colons = (c = sc.scan(/:+/)) ? c.length : 0

      # Locale modifier (%E or %O)
      locale_mod = sc.scan(/[EO]/)&.ord

      spec = sc.getch&.ord

      # Colons are valid only for %z and at most three of them. Any other use
      # (a colon before the width, a colon with a non-z spec, or four or more
      # colons) is invalid, and the C extension emits the original text
      # literally rather than formatting it.
      if colons > 0 && (spec != 122 || colons > 3)
        result << '%' << fmt[spec_start...sc.charpos]
        next
      end

      # A locale modifier (%E / %O) is only valid for certain conversions.
      # When it is not, the C extension emits the original text verbatim,
      # including any width, colons, and flags, so restore and emit it here.
      if locale_mod
        valid = locale_mod == 69 ? STRFTIME_E_VALID_BYTES : STRFTIME_O_VALID_BYTES  # 69='E'
        unless valid.include?(spec)
          result << '%' << fmt[spec_start...sc.charpos]
          next
        end
      end

      # Inline fast path: no flags, no width, no locale mod, no colons
      if flags == 0 && width.nil? && locale_mod.nil? && colons == 0
        fast = fast_spec(spec)
        if fast
          result << fast
          next
        end
      end

      result << format_spec_b(spec, flags, colons, width)
    end

    result
  end

  # Format "%a %b %e %H:%M:%S %Y" directly (used by %c expansion and asctime)
  def fmt_asctime_str
    w = (@jd + 1) % 7
    d = @day
    d_s = d < 10 ? " #{d}" : d.to_s
    y = @year
    y_s = y >= 1000 ? y.to_s : (y >= 0 ? format('%04d', y) : format('-%04d', -y))
    "#{ASCTIME_PREFIX[w][@month]}#{d_s} #{strftime_hms} #{y_s}"
  end

  # Inline fast path for common specs with default formatting (no flags/width)
  def fast_spec(spec)
    case spec
    when 89  # 'Y'
      internal_civil unless @year
      y = @year
      if y >= 1000
        y.to_s
      elsif y >= 0
        format('%04d', y)
      else
        format('-%04d', -y)
      end
    when 109 # 'm'
      internal_civil unless @year
      PAD2[@month]
    when 100 # 'd'
      internal_civil unless @year
      PAD2[@day]
    when 101 # 'e'
      internal_civil unless @year
      d = @day
      d < 10 ? " #{d}" : d.to_s
    when 72  # 'H'
      PAD2[internal_hour]
    when 77  # 'M'
      PAD2[internal_min]
    when 83  # 'S'
      PAD2[internal_sec]
    when 97  # 'a'
      ABBR_DAYNAMES[(@jd + 1) % 7]
    when 65  # 'A'
      DAYNAMES[(@jd + 1) % 7]
    when 98, 104  # 'b', 'h'
      internal_civil unless @year
      ABBR_MONTHNAMES[@month]
    when 66  # 'B'
      internal_civil unless @year
      MONTHNAMES[@month]
    when 112 # 'p'
      internal_hour < 12 ? 'AM' : 'PM'
    when 80  # 'P'
      internal_hour < 12 ? 'am' : 'pm'
    when 37  # '%'
      '%'
    when 110 # 'n'
      "\n"
    when 116 # 't'
      "\t"
    when 106 # 'j'
      yd = yday
      if yd < 10
        "00#{yd}"
      elsif yd < 100
        "0#{yd}"
      else
        yd.to_s
      end
    when 119 # 'w'
      ((@jd + 1) % 7).to_s
    when 117 # 'u'
      cwday.to_s
    when 121 # 'y'
      internal_civil unless @year
      PAD2[@year % 100]
    when 90  # 'Z'
      zone_str
    when 122 # 'z'
      strftime_zoff
    when 115 # 's'
      ((@jd - 2440588) * 86400 - of_seconds + internal_hour * 3600 + internal_min * 60 + internal_sec).to_s
    # Composite specs — inline expansion
    when 99  # 'c'
      internal_civil unless @year
      fmt_asctime_str
    when 70  # 'F'
      internal_civil unless @year
      fast_ymd
    when 84, 88  # 'T', 'X'
      strftime_hms
    when 68, 120  # 'D', 'x'
      internal_civil unless @year
      "#{PAD2[@month]}/#{PAD2[@day]}/#{PAD2[@year % 100]}"
    when 82  # 'R'
      strftime_hm
    when 114  # 'r'
      strftime_r12
    else
      nil  # fall through to format_spec_b
    end
  end

  # Full spec handling with bitmask flags (called when fast_spec returns nil or flags/width present)
  def format_spec_b(spec, flags, colons, width)
    case spec
    when 89, 71  # 'Y', 'G'
      y = spec == 89 ? year : cwyear
      fmt_year(y, width, flags)
    when 67  # 'C'
      cent = year.div(100)
      pad_num(cent, width || 2, flags, zero: true)
    when 121 # 'y'
      pad_num(year % 100, width || 2, flags, zero: true)
    when 103 # 'g'
      pad_num(cwyear % 100, width || 2, flags, zero: true)
    when 109 # 'm'
      pad_num(month, width || 2, flags, zero: true)
    when 100 # 'd'
      pad_num(day, width || 2, flags, zero: true)
    when 101 # 'e'
      pad_num(day, width || 2, flags, zero: false)
    when 106 # 'j'
      pad_num(yday, width || 3, flags, zero: true)
    when 72  # 'H'
      pad_num(internal_hour, width || 2, flags, zero: true)
    when 107 # 'k'
      pad_num(internal_hour, width || 2, flags, zero: false)
    when 73  # 'I'
      h = internal_hour % 12
      h = 12 if h == 0
      pad_num(h, width || 2, flags, zero: true)
    when 108 # 'l'
      h = internal_hour % 12
      h = 12 if h == 0
      pad_num(h, width || 2, flags, zero: false)
    when 77  # 'M'
      pad_num(internal_min, width || 2, flags, zero: true)
    when 83  # 'S'
      pad_num(internal_sec, width || 2, flags, zero: true)
    when 76  # 'L'
      w = width || 3
      ms = (sec_frac * (10**w)).floor
      ms.to_s.rjust(w, '0')
    when 78  # 'N'
      w = width || 9
      ns = (sec_frac * (10**w)).floor
      ns.to_s.rjust(w, '0')
    when 115 # 's'
      unix = (@jd - 2440588) * 86400 - of_seconds +
             internal_hour * 3600 + internal_min * 60 + internal_sec
      pad_num(unix, width || 1, flags, zero: true)
    when 81  # 'Q'
      ms = ((@jd - 2440588) * 86400 - of_seconds +
            internal_hour * 3600 + internal_min * 60 + internal_sec) * 1000 +
           (sec_frac * 1000).floor
      pad_num(ms, width || 1, flags, zero: true)
    when 65  # 'A'
      fmt_str(DAYNAMES[wday], width, flags)
    when 97  # 'a'
      fmt_str(ABBR_DAYNAMES[wday], width, flags)
    when 66  # 'B'
      fmt_str(MONTHNAMES[month], width, flags)
    when 98, 104  # 'b', 'h'
      fmt_str(ABBR_MONTHNAMES[month], width, flags)
    when 112 # 'p'
      fmt_str(internal_hour < 12 ? 'AM' : 'PM', width, flags)
    when 80  # 'P'
      fmt_str(internal_hour < 12 ? 'am' : 'pm', width, flags)
    when 90  # 'Z'
      fmt_str(zone_str, width, flags)
    when 122 # 'z'
      fmt_z(colons, width, flags)
    when 117 # 'u'
      pad_num(cwday, width || 1, flags, zero: true)
    when 119 # 'w'
      pad_num(wday, width || 1, flags, zero: true)
    when 85  # 'U'
      pad_num(week_number(0), width || 2, flags, zero: true)
    when 87  # 'W'
      pad_num(week_number(1), width || 2, flags, zero: true)
    when 86  # 'V'
      pad_num(cweek, width || 2, flags, zero: true)
    when 37  # '%'
      fmt_str('%', width, flags)
    else
      # Try composite (includes '+', see STRFTIME_COMPOSITE_BYTE)
      expansion = STRFTIME_COMPOSITE_BYTE[spec]
      if expansion
        s = internal_strftime(expansion)
        fmt_str(s, width, flags)
      else
        "%#{spec&.chr}"
      end
    end
  end

  # Format year (handles negative years and all flag variants)
  def fmt_year(y, width, flags)
    # The result is always numeric (digits with an optional leading '-'), so the
    # upper/chcase flags are no-ops and need no handling here.
    sign = y < 0 ? '-' : ''
    if flags & FL_LEFT != 0
      sign + y.abs.to_s
    else
      w = width || (y < 0 ? 5 : 4)
      if flags & FL_SPACE != 0
        (sign + y.abs.to_s).rjust(w, ' ')
      else
        sign + y.abs.to_s.rjust(w - sign.length, '0')
      end
    end
  end

  def pad_num(n, default_w, flags, zero: nil)
    sign = n < 0 ? '-' : ''
    abs  = n.abs.to_s
    w    = default_w

    if flags & FL_LEFT != 0
      sign + abs
    elsif flags & FL_SPACE != 0 || (flags & FL_ZERO == 0 && zero == false)
      # Space padding right-justifies the sign together with the digits, so a
      # negative value pads as "  -1", not "-  1" (matching the C extension).
      (sign + abs).rjust(w, ' ')
    else
      pad = (flags & FL_ZERO != 0 || zero) ? '0' : ' '
      sign + abs.rjust([w - sign.length, abs.length].max, pad)
    end
  end

  def fmt_str(s, width, flags)
    if flags & FL_CHCASE != 0
      s = (s == s.upcase) ? s.downcase : s.upcase
    elsif flags & FL_UPPER != 0
      s = s.upcase
    end
    # Padding applies only when not left-justified and a width is given; the
    # case conversions above already return fresh strings, so no defensive dup
    # is needed for the paths that return s unchanged.
    if flags & FL_LEFT == 0 && width
      pad = flags & FL_ZERO != 0 ? '0' : ' '
      s = s.rjust(width, pad)
    end
    s
  end

  # Week number (0=first partial week)
  def week_number(ws)
    yd   = yday
    wd   = wday  # 0=Sun
    if ws == 1
      # Monday-based
      wd = wd == 0 ? 6 : wd - 1
    end
    (yd - wd + 6).div(7)
  end

  # Format %z with colons variant and GNU extension flag support
  def fmt_z(colons, width, flags)
    of   = of_seconds
    sign = of < 0 ? '-' : '+'
    abs  = of.abs
    hh   = abs / 3600
    mm   = (abs % 3600) / 60
    ss   = abs % 60

    no_lead = flags & (FL_LEFT | FL_SPACE) != 0

    # The colons variants differ between the leading-zero and no-leading-zero
    # forms only in how the hour is padded ('%02d' vs '%d'); the minutes and
    # seconds are always two digits, so decide the hour rendering once here.
    hh_s = (no_lead || hh >= 10) ? hh.to_s : "0#{hh}"
    mm_s = mm < 10 ? "0#{mm}" : mm.to_s
    ss_s = ss < 10 ? "0#{ss}" : ss.to_s

    s = case colons
        when 1
          "#{sign}#{hh_s}:#{mm_s}"
        when 2
          "#{sign}#{hh_s}:#{mm_s}:#{ss_s}"
        when 3
          if ss != 0
            "#{sign}#{hh_s}:#{mm_s}:#{ss_s}"
          elsif mm != 0
            "#{sign}#{hh_s}:#{mm_s}"
          else
            "#{sign}#{hh_s}"
          end
        when 0
          "#{sign}#{hh_s}#{mm_s}"
        else
          "#{sign}#{hh_s}:#{mm_s}"
        end

    if flags & FL_SPACE != 0
      # '_' pads with spaces. Without '-' it pads to at least the width the
      # zero-padded form would occupy (the hour is the only field that loses
      # its leading zero); '-' suppresses that automatic minimum width.
      min_w = (flags & FL_LEFT != 0) ? 0 : s.length + (hh < 10 ? 1 : 0)
      s.rjust([width || 0, min_w].max, ' ')
    elsif width
      # The default and '-' both zero-pad to the given width; for %z, '-' does
      # not left-justify.
      sign + s[1..].rjust(width - 1, '0')
    else
      s
    end
  end

  # Fast path helper: format '%Y-%m-%d' without allocation overhead
  def fast_ymd
    y = @year
    suffix = MONTH_DAY_SUFFIX[@month][@day]
    if y >= 1000
      y.to_s << suffix
    elsif y >= 0
      format('%04d', y) << suffix
    else
      format('-%04d', -y) << suffix
    end
  end

  # Helpers for DateTime override
  def internal_hour
    0
  end

  def internal_min
    0
  end

  def internal_sec
    0
  end

  def sec_frac
    0r
  end

  def of_seconds
    0
  end

  def zone_str
    '+00:00'
  end

  # Time-component strings for the strftime fast paths. A Date has no time, so
  # each is a fixed literal here; DateTime overrides them with the computed
  # values. Keeping them as (overridable) methods lets each class supply its
  # own without a per-call instance_of? test inside the formatter.
  def strftime_hms   # %T, %X, %H:%M:%S
    '00:00:00'
  end

  def strftime_hm    # %R
    '00:00'
  end

  def strftime_r12   # %r
    '12:00:00 AM'
  end

  def strftime_zoff  # %z (numeric +HHMM offset)
    '+0000'
  end

end
