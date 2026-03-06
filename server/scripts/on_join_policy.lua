local RESTRICTED_ROOMS = {
  vip_lounge = true,
  staff_ops = true,
}

function on_join(ctx)
  if not ctx or not ctx.room then
    return { decision = "pass" }
  end

  if not RESTRICTED_ROOMS[ctx.room] then
    return { decision = "pass" }
  end

  if ctx.user and server.is_user_banned(ctx.user) then
    return {
      decision = "deny",
      reason = "restricted rooms are unavailable for banned users",
    }
  end

  local occupants = server.get_room_users(ctx.room)
  if #occupants >= 50 then
    return {
      decision = "deny",
      reason = "restricted room is full",
    }
  end

  return { decision = "pass" }
end
