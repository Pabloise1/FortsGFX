-- FortsGFX test bench marker "fortsgfx_capture_13_device_smoke": an invisible sprite FortsGFX answers with a capture.
LifeSpan = 0.6

Sprites =
{
    {
        Name = "fortsgfx_capture_13_device_smoke",
        States =
        {
            Normal =
            {
                Frames =
                {
                    { texture = "maps/fortsgfx testbench/effects/media/fortsgfx_capture_13_device_smoke.png" },
                    duration = 1,
                },
                NextState = "Normal",
            },
        },
    },
}

Effects =
{
    {
        Type = "sprite",
        TimeToTrigger = 0,
        LocalPosition = { x = 0, y = 0, z = 0 },
        LocalVelocity = { x = 0, y = 0, z = 0 },
        Acceleration = { x = 0, y = 0, z = 0 },
        Drag = 0.0,
        Sprite = "fortsgfx_capture_13_device_smoke",
        Additive = false,
        TimeToLive = 0.5,
        Angle = 0,
        InitialSize = 2,
        ExpansionRate = 0,
        AngularVelocity = 0,
        RandomAngularVelocityMagnitude = 0,
        Colour1 = { 255, 255, 255, 255 },
        Colour2 = { 255, 255, 255, 255 },
    },
}
