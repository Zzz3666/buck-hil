# 5. 上位机软件设计 (C# / WPF)

## 5.1 技术选型

| 组件 | 选择 | 理由 |
|------|------|------|
| UI 框架 | WPF (Windows Presentation Foundation) | 数据绑定成熟，渲染性能好 |
| .NET 版本 | .NET 8.0 | LTS 版本，性能提升显著 |
| 图表库 | OxyPlot.Wpf | 轻量，支持高速刷新，MIT 协议 |
| 通信 | System.Net.Sockets (原生) | 零依赖，完全控制 |
| 数据存储 | System.IO + BinaryWriter | 环形缓冲，预分配，零 GC |
| 日志 | Serilog | 结构化日志，支持文件/控制台 |
| 配置 | appsettings.json | 标准 .NET 配置 |

## 5.2 线程模型 (铁律)

```
┌─────────────────────────────────────────────────────────────────┐
│                     Main Thread (UI Thread)                      │
│  Dispatcher Priority = Normal                                    │
│                                                                  │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌───────────────┐   │
│  │MainWindow│  │波形控件   │  │参数面板   │  │状态指示器      │   │
│  │.xaml     │  │OxyPlot   │  │TextBox   │  │StatusBar      │   │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └───────┬───────┘   │
│       │              │              │               │            │
│  ┌────┴──────────────┴──────────────┴───────────────┴────┐      │
│  │             DataBinding (INotifyPropertyChanged)       │      │
│  │             绝不直接调用 Socket, 文件 I/O, 锁           │      │
│  └──────────────────────────┬────────────────────────────┘      │
│                             │                                    │
│   DispatcherTimer (33ms) ───┤ 定时从 Model 拉取最新数据到 UI     │
│                             │                                    │
└─────────────────────────────┼────────────────────────────────────┘
                              │
              ┌───────────────┼───────────────┐
              │               │               │
              ▼               ▼               ▼
    ┌─────────────┐  ┌─────────────┐  ┌─────────────┐
    │ConcurrentQueue│ │ConcurrentQueue│ │BlockingColl.│
    │<CmdMessage>  │ │<StatusUpdate>│ │<DataChunk>  │
    └──────┬──────┘  └──────┬──────┘  └──────┬──────┘
           │                │                │
┌──────────┴────────────────┴────────────────┴────────────────────┐
│                  Communication Thread (Background)                │
│  Thread Priority = AboveNormal                                   │
│                                                                  │
│  ┌───────────────┐  ┌────────────────┐  ┌───────────────────┐   │
│  │ TcpClient     │  │ FrameParser    │  │ RecvLoop (async)  │   │
│  │ Connect/Reconn│  │ State Machine  │  │ while(true)       │   │
│  └───────┬───────┘  └───────┬────────┘  └─────────┬─────────┘   │
│          │                  │                      │              │
│          │         ┌────────┴────────┐             │              │
│          │         │ 完整帧 → 分发    │◄────────────┘              │
│          │         │ CMD 路由        │                            │
│          │         └────────┬────────┘                            │
│          │                  │                                     │
│          │    ┌─────────────┼─────────────┐                      │
│          │    ▼             ▼             ▼                      │
│          │ [响应→UI队列] [数据→环形缓冲] [心跳检测]               │
│          │                                                        │
│   绝不：操作 UI 控件, 调用 Dispatcher.Invoke (同步阻塞)            │
└──────────┴───────────────────────────────────────────────────────┘
                              │
                              │ BlockingCollection<DataChunk>
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│               Data Processing Thread (Background)                 │
│                                                                  │
│  • 环形缓冲管理 (预分配 10M samples, 零动态分配)                   │
│  • 触发数据重组 (SEQ# 排序, 缺失检测)                             │
│  • 降采样 (用于长期显示)                                          │
│  • 文件存储 (异步 FileStream.WriteAsync)                          │
└──────────────────────────────────────────────────────────────────┘
```

## 5.3 关键类设计

### 5.3.1 FrameParser — 帧解析状态机

```csharp
/// <summary>
/// 逐字节帧解析器，线程安全，支持粘包/半包。
/// 与 PS 端协议状态机完全对称。
/// </summary>
public class FrameParser
{
    private enum State { Sync1, Sync2, Cmd, LenH, LenL, Payload, CrcH, CrcL, Tail }

    private State _state = State.Sync1;
    private byte _cmd;
    private ushort _len;
    private ushort _payloadIdx;
    private byte[] _payload = new byte[65536];
    private ushort _crcCalculated;
    private ushort _crcReceived;
    private long _lastByteTicks;

    private const ushort CRC16_POLY = 0x1021;
    private const int FRAME_TIMEOUT_MS = 500;

    /// <summary>
    /// 每收到一个字节调用一次。
    /// 返回 null 表示继续等待，返回 Frame 对象表示完整帧已解析。
    /// </summary>
    public Frame? FeedByte(byte b)
    {
        _lastByteTicks = Environment.TickCount64;

        switch (_state)
        {
            case State.Sync1:
                if (b == 0xAA) _state = State.Sync2;
                break;

            case State.Sync2:
                if (b == 0x55)
                {
                    _state = State.Cmd;
                    _crcCalculated = Crc16Init();
                    _crcCalculated = Crc16Update(_crcCalculated, b);
                }
                else _state = State.Sync1;
                break;

            case State.Cmd:
                _cmd = b;
                _crcCalculated = Crc16Update(_crcCalculated, b);
                _state = State.LenH;
                break;

            case State.LenH:
                _len = (ushort)(b << 8);
                _crcCalculated = Crc16Update(_crcCalculated, b);
                _state = State.LenL;
                break;

            case State.LenL:
                _len |= b;
                _crcCalculated = Crc16Update(_crcCalculated, b);
                if (_len > 65535) { _state = State.Sync1; return null; }
                _payloadIdx = 0;
                _state = _len == 0 ? State.CrcH : State.Payload;
                break;

            case State.Payload:
                _payload[_payloadIdx++] = b;
                _crcCalculated = Crc16Update(_crcCalculated, b);
                if (_payloadIdx >= _len) _state = State.CrcH;
                break;

            case State.CrcH:
                _crcReceived = (ushort)(b << 8);
                _state = State.CrcL;
                break;

            case State.CrcL:
                _crcReceived |= b;
                _state = State.Tail;
                break;

            case State.Tail:
                _state = State.Sync1;
                if (b == 0x55 && _crcCalculated == _crcReceived)
                    return new Frame(_cmd, _payload.AsSpan(0, _len).ToArray());
                break;
        }

        // 超时保护
        if (Environment.TickCount64 - _lastByteTicks > FRAME_TIMEOUT_MS)
            _state = State.Sync1;

        return null;
    }

    public void FeedBytes(ReadOnlySpan<byte> bytes)
    {
        foreach (byte b in bytes)
        {
            var frame = FeedByte(b);
            if (frame.HasValue)
                OnFrameReceived?.Invoke(frame.Value);
        }
    }

    public event Action<Frame>? OnFrameReceived;

    private static ushort Crc16Init() => 0xFFFF;
    private static ushort Crc16Update(ushort crc, byte b)
    {
        crc ^= (ushort)(b << 8);
        for (int i = 0; i < 8; i++)
        {
            if ((crc & 0x8000) != 0)
                crc = (ushort)((crc << 1) ^ CRC16_POLY);
            else
                crc <<= 1;
        }
        return crc;
    }
}
```

### 5.3.2 CommunicationThread — 通信线程

```csharp
public class CommunicationThread : IDisposable
{
    private readonly FrameParser _parser = new();
    private TcpClient? _tcp;
    private NetworkStream? _stream;
    private CancellationTokenSource? _cts;
    private Task? _recvTask;

    // 输出队列 → UI 线程 / 数据处理线程 (无锁)
    private readonly ConcurrentQueue<Frame> _responseQueue = new();
    private readonly BlockingCollection<DataChunk> _dataQueue = new(boundedCapacity: 100);

    public BlockingCollection<DataChunk> DataQueue => _dataQueue;
    public ConcurrentQueue<Frame> ResponseQueue => _responseQueue;

    public event Action<ConnectionState>? ConnectionStateChanged;

    public async Task ConnectAsync(string host, int port)
    {
        while (!_cts!.IsCancellationRequested)
        {
            try
            {
                _tcp = new TcpClient { NoDelay = true, ReceiveTimeout = 2000 };
                await _tcp.ConnectAsync(host, port);
                _stream = _tcp.GetStream();
                ConnectionStateChanged?.Invoke(ConnectionState.Connected);

                await RecvLoopAsync(_stream, _cts.Token);
            }
            catch (Exception ex)
            {
                Log.Error(ex, "连接失败，3 秒后重试");
                ConnectionStateChanged?.Invoke(ConnectionState.Disconnected);
                await Task.Delay(3000, _cts.Token);
            }
            finally
            {
                _stream?.Dispose();
                _tcp?.Dispose();
            }
        }
    }

    private async Task RecvLoopAsync(NetworkStream stream, CancellationToken ct)
    {
        var buffer = new byte[8192];  // 8KB 接收缓冲
        _parser.OnFrameReceived += frame =>
        {
            if (IsResponseFrame(frame.Cmd))
                _responseQueue.Enqueue(frame);
            else if (frame.Cmd == 0x11 || frame.Cmd == 0x12)
                _dataQueue.TryAdd(DataChunk.FromFrame(frame));
        };

        while (!ct.IsCancellationRequested)
        {
            int bytesRead = await stream.ReadAsync(buffer, 0, buffer.Length, ct);
            if (bytesRead == 0) break;  // 连接关闭

            _parser.FeedBytes(buffer.AsSpan(0, bytesRead));
        }
    }

    public async Task SendFrameAsync(Frame frame)
    {
        if (_stream == null) throw new InvalidOperationException("未连接");

        byte[] raw = frame.Serialize();  // 内部计算 CRC 并组装完整帧
        await _stream.WriteAsync(raw, 0, raw.Length);
        await _stream.FlushAsync();
    }

    /// <summary>
    /// 发送命令并等待应答（带超时 + 重试）
    /// </summary>
    public async Task<Frame> SendCommandAsync(
        byte cmd, ReadOnlyMemory<byte> payload,
        int timeoutMs = 500, int maxRetries = 3)
    {
        for (int attempt = 0; attempt < maxRetries; attempt++)
        {
            var request = new Frame(cmd, payload.ToArray());
            await SendFrameAsync(request);

            // 等待应答（基于 SEQ 匹配或简单 FIFO）
            using var responseCts = new CancellationTokenSource(timeoutMs);
            try
            {
                while (!responseCts.Token.IsCancellationRequested)
                {
                    if (_responseQueue.TryDequeue(out var response))
                        return response;

                    await Task.Delay(1, responseCts.Token);
                }
            }
            catch (OperationCanceledException) { }

            Log.Warning("命令 0x{0:X2} 超时，重试 {1}/{2}", cmd, attempt + 1, maxRetries);
        }

        throw new TimeoutException($"命令 0x{cmd:X2} 无应答");
    }
}
```

### 5.3.3 RingBuffer — 高速环形缓冲

```csharp
/// <summary>
/// 预分配环形缓冲，追加数据不产生内存分配。
/// 用于高速波形数据暂存，供 OxyPlot 渲染。
/// </summary>
public class RingBuffer<T> where T : struct
{
    private readonly T[] _buffer;
    private long _writePos;

    public RingBuffer(int capacity)
    {
        _buffer = new T[capacity];
        _writePos = 0;
    }

    public void Append(ReadOnlySpan<T> data)
    {
        int capacity = _buffer.Length;
        int writeIndex = (int)(_writePos % capacity);
        int len = data.Length;

        if (writeIndex + len <= capacity)
        {
            // 单次拷贝
            data.CopyTo(_buffer.AsSpan(writeIndex));
        }
        else
        {
            // 跨边界：分两段
            int firstPart = capacity - writeIndex;
            data.Slice(0, firstPart).CopyTo(_buffer.AsSpan(writeIndex));
            data.Slice(firstPart).CopyTo(_buffer.AsSpan(0));
        }

        Interlocked.Add(ref _writePos, len);
    }

    public ReadOnlySpan<T> GetRecent(int count)
    {
        long pos = Interlocked.Read(ref _writePos);
        int capacity = _buffer.Length;
        int start = (int)((pos - count) % capacity);
        if (start < 0) start += capacity;

        if (start + count <= capacity)
            return _buffer.AsSpan(start, count);

        // 跨边界 → 需要拷贝 (罕见情况)
        var result = new T[count];
        int firstPart = capacity - start;
        _buffer.AsSpan(start, firstPart).CopyTo(result);
        _buffer.AsSpan(0, count - firstPart).CopyTo(result.AsSpan(firstPart));
        return result;
    }
}
```

### 5.3.4 MainWindow — UI 线程绑定

```csharp
public partial class MainWindow : Window
{
    private readonly CommunicationThread _comm = new();
    private readonly RingBuffer<double> _ringVout = new(1_000_000);  // 1M points
    private readonly RingBuffer<double> _ringIL   = new(1_000_000);

    private readonly DispatcherTimer _renderTimer;

    public MainWindow()
    {
        InitializeComponent();

        // 33ms ≈ 30 FPS 渲染
        _renderTimer = new DispatcherTimer(
            TimeSpan.FromMilliseconds(33),
            DispatcherPriority.Normal,
            OnRenderTick,
            Dispatcher);

        // 启动通信线程
        Task.Run(() => _comm.ConnectAsync("192.168.1.100", 5000));

        // 启动数据处理线程
        Task.Run(DataProcessLoop);
    }

    /// <summary>
    /// 渲染回调 — 在 UI 线程执行，只读缓冲。
    /// 绝不调任何阻塞操作。
    /// </summary>
    private void OnRenderTick(object? sender, EventArgs e)
    {
        // 获取最近 10000 点 → 生成 OxyPlot DataPoint[]
        var voutData = _ringVout.GetRecent(10000);
        var ilData = _ringIL.GetRecent(10000);

        // 更新 Plot (OxyPlot 的 RefreshPlot 内部双缓冲)
        var voutSeries = (LineSeries)PlotVout.Series[0];
        voutSeries.Points.Clear();
        for (int i = 0; i < voutData.Length; i++)
            voutSeries.Points.Add(new DataPoint(i, voutData[i]));

        var ilSeries = (LineSeries)PlotIL.Series[0];
        ilSeries.Points.Clear();
        for (int i = 0; i < ilData.Length; i++)
            ilSeries.Points.Add(new DataPoint(i, ilData[i]));

        PlotVout.InvalidatePlot(true);
        PlotIL.InvalidatePlot(true);
    }

    /// <summary>
    /// 数据处理循环 — 后台线程。
    /// 从通信线程消费 DataChunk，推入环形缓冲。
    /// </summary>
    private void DataProcessLoop()
    {
        foreach (var chunk in _comm.DataQueue.GetConsumingEnumerable())
        {
            // 写入环形缓冲
            _ringVout.Append(chunk.VoutData);
            _ringIL.Append(chunk.ILData);

            // 异步存储到文件 (不阻塞)
            // await FileStorage.WriteAsync(chunk);
        }
    }
}
```

## 5.4 参数配置面板

```csharp
/// <summary>
/// 参数 ViewModel — 双向绑定。
/// 用户修改参数 → 异步发送 CMD_WRITE_PARAM → 等待应答 → 更新状态。
/// </summary>
public class ParameterViewModel : INotifyPropertyChanged
{
    private readonly CommunicationThread _comm;

    private double _inductance = 100.0;  // μH
    public double Inductance
    {
        get => _inductance;
        set
        {
            if (SetProperty(ref _inductance, value))
                _ = WriteParamAsync(0x0001, (uint)(value * 1000)); // μH → nH
        }
    }

    private double _capacitance = 100.0;  // μF
    public double Capacitance
    {
        get => _capacitance;
        set
        {
            if (SetProperty(ref _capacitance, value))
                _ = WriteParamAsync(0x0002, (uint)(value * 1_000_000)); // μF → pF
        }
    }

    private double _loadResistance = 10.0;  // Ω
    public double LoadResistance
    {
        get => _loadResistance;
        set
        {
            if (SetProperty(ref _loadResistance, value))
                _ = WriteParamAsync(0x0003, (uint)(value * 1000)); // Ω → mΩ
        }
    }

    private async Task WriteParamAsync(ushort id, uint value)
    {
        try
        {
            var payload = new byte[6];
            payload[0] = (byte)(id >> 8);
            payload[1] = (byte)(id & 0xFF);
            payload[2] = (byte)(value >> 24);
            payload[3] = (byte)(value >> 16);
            payload[4] = (byte)(value >> 8);
            payload[5] = (byte)(value & 0xFF);

            var response = await _comm.SendCommandAsync(0x01, payload);

            // 检查应答: CMD_PARAM_RESP + 相同 ID
            if (response.Cmd == 0x10 && response.Payload[0] == payload[0]
                                     && response.Payload[1] == payload[1])
            {
                StatusMessage = "参数写入成功";
            }
            else
            {
                StatusMessage = "参数写入失败: 应答不匹配";
            }
        }
        catch (TimeoutException)
        {
            StatusMessage = "参数写入失败: 超时";
        }
    }

    public string? StatusMessage { get; private set; }
    public event PropertyChangedEventHandler? PropertyChanged;
}
```

## 5.5 文件存储

```csharp
/// <summary>
/// 异步文件写入器 — 不阻塞通信/UI 线程。
/// 二进制格式存储，紧凑且可直接回放。
/// </summary>
public class FileStorage
{
    private readonly SemaphoreSlim _writeLock = new(1, 1);
    private FileStream? _file;
    private readonly string _baseDir;

    public FileStorage(string baseDir)
    {
        _baseDir = baseDir;
    }

    /// <summary>
    /// 开始新记录会话 (自动命名: YYYYMMDD_HHMMSS.bin)
    /// </summary>
    public async Task BeginSessionAsync()
    {
        await _writeLock.WaitAsync();
        try
        {
            var name = $"{DateTime.Now:yyyyMMdd_HHmmss}.bin";
            _file = new FileStream(
                Path.Combine(_baseDir, name),
                FileMode.Create,
                FileAccess.Write,
                FileShare.Read,
                65536,  // 64KB buffer
                FileOptions.Asynchronous);
        }
        finally { _writeLock.Release(); }
    }

    /// <summary>
    /// 追加数据块 (异步)
    /// </summary>
    public async Task WriteChunkAsync(DataChunk chunk)
    {
        await _writeLock.WaitAsync();
        try
        {
            if (_file == null) return;

            // 格式: [4B seq][2B count][N × 8B samples]
            var header = new byte[6];
            BitConverter.TryWriteBytes(header.AsSpan(0, 4), chunk.Seq);
            BitConverter.TryWriteBytes(header.AsSpan(4, 2), chunk.Count);
            await _file.WriteAsync(header);

            await _file.WriteAsync(chunk.RawData);
            await _file.FlushAsync();
        }
        finally { _writeLock.Release(); }
    }
}
```

## 5.6 测试脚本引擎

```csharp
/// <summary>
/// 轻量级测试序列引擎。
/// 支持: 设置参数 → 延时 → 触发 → 等待数据 → 设置参数 → ...
/// </summary>
public class TestScriptEngine
{
    public enum StepType { SetParam, Delay, StartSim, StopSim, ArmTrigger, WaitTrigger, Verify }

    public record Step(StepType Type, Dictionary<string, object>? Args = null);

    private readonly CommunicationThread _comm;
    private readonly ParameterViewModel _params;

    public async Task RunAsync(IEnumerable<Step> steps, CancellationToken ct)
    {
        foreach (var step in steps)
        {
            ct.ThrowIfCancellationRequested();

            switch (step.Type)
            {
                case StepType.SetParam:
                    // 从 step.Args 提取 id/value 并发送
                    break;
                case StepType.Delay:
                    await Task.Delay((int)(step.Args!["ms"]), ct);
                    break;
                case StepType.StartSim:
                    await _comm.SendCommandAsync(0x05, new byte[] { 0x01 });
                    break;
                case StepType.ArmTrigger:
                    // 配置触发条件并下发
                    break;
                case StepType.WaitTrigger:
                    // 等待触发数据到达
                    break;
            }
        }
    }
}
```

## 5.7 项目结构

```
host/
├── BuckHil.sln
├── BuckHil/
│   ├── BuckHil.csproj
│   ├── App.xaml
│   ├── App.xaml.cs
│   ├── MainWindow.xaml
│   ├── MainWindow.xaml.cs
│   ├── Communication/
│   │   ├── FrameParser.cs
│   │   ├── CommunicationThread.cs
│   │   ├── DataChunk.cs
│   │   └── ConnectionState.cs
│   ├── Model/
│   │   ├── RingBuffer.cs
│   │   ├── ParameterViewModel.cs
│   │   └── StatusViewModel.cs
│   ├── Storage/
│   │   └── FileStorage.cs
│   ├── Scripting/
│   │   └── TestScriptEngine.cs
│   └── Views/
│       ├── WaveformView.xaml
│       ├── ParameterPanel.xaml
│       └── ScriptEditor.xaml
└── protocol/
    └── Protocol.cs         # 命令常量、Frame 结构体、CRC16
```

## 5.8 构建与部署

```xml
<!-- BuckHil.csproj (简化) -->
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>WinExe</OutputType>
    <TargetFramework>net8.0-windows</TargetFramework>
    <UseWPF>true</UseWPF>
    <Nullable>enable</Nullable>
    <PublishSingleFile>true</PublishSingleFile>
    <SelfContained>false</SelfContained>
  </PropertyGroup>

  <ItemGroup>
    <PackageReference Include="OxyPlot.Wpf" Version="2.1.*" />
    <PackageReference Include="Serilog.Sinks.File" Version="5.*" />
  </ItemGroup>
</Project>
```

**发布命令**:
```bash
dotnet publish -c Release -r win-x64 --self-contained false -o ./publish
```
