<script>
    import Chart from "chart.js/auto";
    import {onMount} from "svelte";

    function toScatterData(src, interval) {
        let values = [];
        let keys = Object.keys(src)
        let min = Math.min(...keys);
        let max = Math.max(...keys);
        for(let i = min; i <= max; i += interval) {
            let datapoint = src[i];
            if(datapoint === undefined) {
                //console.log("Missing datapoint for element " + (i - min).toString() + "(" + i + ")");
                //console.log(src, src[i]);
                values.push({x: i, y: 0});
            } else {
                values.push({x: i, y: datapoint.msg_count})
            }
        }
        return {values: values, min: min, max: max};
    }

    function drawHistogram(canvasId, color, scatterData) {
        let min = scatterData.min;
        let max = scatterData.max;
        let values = scatterData.values;

        const data = {
            datasets: [{
                label: 'test',
                backgroundColor: color,
                borderColor: color,
                data: values,
            }]
        };
        Chart.defaults.font.size = 16;
        const config = {
            type: 'scatter',
            data: data,
            options: {
                showLine: true,
                elements: {
                    point:{
                        radius: 0
                    }
                },
                plugins: {
                    legend: {
                        display: false
                    },
                    tooltip: {
                        callbacks: {
                            label: function(context) {
                                return new Date(context.parsed.x * 1000).toLocaleTimeString('de-DE') + ": " + context.parsed.y;
                            }
                        }
                    },
                },
                scales: {
                    x: {
                        ticks: {
                            callback: function (value) {
                                return new Date(value * 1000).toLocaleTimeString('de-DE');
                            },
                        },
                        min: min,
                        max: max
                    },
                    y: {
                        min: 0
                    }
                },
            }
        };
        let c = new Chart(
            document.getElementById(canvasId),
            config
        );

        return function(scatterData) {
            c.data.datasets.forEach((dataset) => {
                dataset.data = scatterData.values;
            })
            c.options.scales.x.min = scatterData.min;
            c.options.scales.x.max = scatterData.max;
            c.update();
        }
    }

    let statsPromise = fetch("/statistics");
    let totalMessageCount = 0;
    let retainedMsgCount = 0;
    let retainedMsgBytes = 0;
    let appQueueDepth = 0;
    let appSleepState = "";
    let activeSubscriptions = 0;
    let uptime = 0;
    function onDataReceived(stats) {
        totalMessageCount = stats.total_msg_count
        retainedMsgCount = stats.retained_msg_count
        retainedMsgBytes = stats.retained_msg_size_sum;
        appQueueDepth = stats.app_state_queue_depth;
        if(stats.current_sleep_level === "tens_of_milliseconds") {
            appSleepState = "10ms";
        } else if(stats.current_sleep_level === "milliseconds") {
            appSleepState = "1ms";
        } else if(stats.current_sleep_level === "microseconds") {
            appSleepState = "10µs";
        } else if(stats.current_sleep_level === "yield") {
            appSleepState = "yield";
        } else {
            appSleepState = stats.current_sleep_level;
        }
        activeSubscriptions = Object.entries(stats.active_subscriptions).map((kv) => kv[1]).reduce((sum, a) => sum + a, 0);
        uptime = stats.uptime_seconds;
    }

    let refreshIntervalTimeoutId = 0;
    let graphSeconds;
    let graphMinutes;

    async function updateStatistics() {

        let stats = await (await fetch("/statistics")).json();
        graphSeconds(toScatterData(stats.msg_per_second, 1));
        graphMinutes(toScatterData(stats.msg_per_minute, 60));
        onDataReceived(stats);
    }

    let initDone = false;
    onMount(async () => {
        let stats = await (await statsPromise).json()
        onDataReceived(stats);
        graphSeconds = drawHistogram('messagesPerSecond', 'rgb(255, 99, 132)', toScatterData(stats.msg_per_second, 1));
        graphMinutes = drawHistogram('messagesPerMinute', 'rgb(211,99,255)', toScatterData(stats.msg_per_minute, 60));
        oldRefreshRate = refreshRate;
        refreshIntervalTimeoutId = setInterval(updateStatistics, refreshRate);
        initDone = true;
    });

    export let refreshRate;
    let oldRefreshRate;
    $: if (oldRefreshRate !== refreshRate && initDone) {
        oldRefreshRate = refreshRate;
        (async () => {
            clearInterval(refreshIntervalTimeoutId);
            await updateStatistics();
            refreshIntervalTimeoutId = setInterval(updateStatistics, refreshRate);
        })();

    }
    function formatDuration(seconds) {
        let s = Math.floor(seconds % 60).toString().padStart(2, "0");
        let m = Math.floor((seconds / 60) % 60).toString().padStart(2, "0");
        let h = Math.floor((seconds / 3600) % 24).toString().padStart(2, "0");
        let d = Math.floor((seconds / (3600 * 24)));

        return (d >= 0 ? d + " days " : "") + h + ":" + m + ":" + s;
    }
</script>

<main>
    <div id="numbers">
        <div class="control">
            <div>Version</div>
            <div class="highlight">Alpha</div>
        </div>
        <div class="control">
            <div>Uptime</div>
            <div class="highlight">{formatDuration(uptime)}</div>
        </div>
        <div class="control">
            <div>Total Messages</div>
            <div class="highlight">{totalMessageCount}</div>
        </div>
        <div class="control">
            <div>Active Subscriptions</div>
            <div class="highlight">{activeSubscriptions}</div>
        </div>
        <div class="control">
            <div>Retained Bytes</div>
            <div class="highlight">{retainedMsgBytes}</div>
        </div>
        <div class="control">
            <div>App Queue Depth</div>
            <div class="highlight">{appQueueDepth}</div>
        </div>
        <div class="control">
            <div>Sleep State</div>
            <div class="highlight">{appSleepState}</div>
        </div>
        <div class="control">
            <div>Retained Count</div>
            <div class="highlight">{retainedMsgCount}</div>
        </div>
    </div>
    <div id="graphs">
        <div class="control" >
            <center>Messages per Second</center>
            <canvas id="messagesPerSecond"></canvas>
        </div>
        <div class="control" >
            <center>Messages per Minute</center>
            <canvas id="messagesPerMinute"></canvas>
        </div>
    </div>

</main>

<style>
    #numbers {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
        gap: 20px;
        padding-bottom: 20px;
    }
    #graphs {
        display: grid;
        grid-template-columns: calc(50% - 10px) calc(50% - 10px);
        grid-auto-rows: auto;
        gap: 20px;
        padding-bottom: 50px;
    }
    .control {
        background-color: white;
        border-radius: 10px;
        box-shadow: #9f9f9f 0 0 5px;
        padding: 10px;
        margin: 0;
        font-size: 20px;
    }
    .highlight {
        font-size: 30px;
        font-weight: bold;
    }
    @media only screen and (max-width: 700px) {
        #graphs {
            grid-template-columns: 100%;
        }
        main {
            font-size: 15px;
        }
        .control {
            font-size: 20px;
        }
        .highlight {
            font-size: 25px;
        }
    }
</style>