"use strict";
var vueApp, chart;

function openTab(ident) {
    switch(ident) {
        case "status":
            getStatus();
            break;
        case "env":
            getEnv();
            break;
        case "history":
            getHistory();
            break;
        case "config":
            getConfig();
            break;
    }
    vueApp.activeTab = ident;
}

function setLoader(name, state) {
    var elements = document.getElementsByName(name);
    for (let element of elements) {
        let cn = "loader";
        switch (state) {
            case "loading":
                cn += " loader-loading";
                break;
            case "error":
                cn += " loader-error";
                break;
            case "done":
                break;
        }
        element.className = cn;
    }
}

function getStatus() {
    setLoader("status-loader", "loading");
    axios.get("/api/status").then(function(response) {
        vueApp.properties.length = 0; // clear old data
        for (let prop of response.data.status) {
            if (prop.type === "seconds") {
                prop.displayValue = humanizeDuration(prop.value*1000);
            }
            vueApp.properties.push(prop);
        }
        vueApp.properties.hostname = response.data.hostname;
        vueApp.properties.ipaddr = response.data.ipaddr;
        setLoader("status-loader", "done");
    }).catch(function(error) {
        setLoader("status-loader", "error");
    });
}

function getConfig(save = false) {
    setLoader("config-loader", "loading");
    let promise;
    if (save) {
        let obj = {
            historyInterval: vueApp.localConfig.historyInterval,
            sleepOnReset: vueApp.localConfig.sleepOnReset
        };
        promise = axios.put("/api/config", obj);
    } else {
        promise = axios.get("/api/config");
    }
    promise.then(function(response) {
        if ("historyInterval" in response.data) {
            let value = response.data.historyInterval;
            vueApp.serverConfig.historyInterval = value;
            vueApp.serverConfig.standard = false;
            for (let option of vueApp.historyIntervalOptions) {
                if (option.value == value) {
                    vueApp.serverConfig.standard = true;
                    break;
                }
            }
            vueApp.localConfig.historyInterval = value;
        }
        if ("sleepOnReset" in response.data) {
            let value = response.data.sleepOnReset ? true : false;
            vueApp.localConfig.sleepOnReset = vueApp.serverConfig.sleepOnReset = value;
        }
        setLoader("config-loader", "done");
    }).catch(function(error) {
        setLoader("config-loader", "error");
    });
}

function getEnv() {
    setLoader("env-loader", "loading");
    axios.get("/api/env").then(function(response) {
        var env = response.data;
        env.time = new Date(env.time * 1000);
        vueApp.env = env;
        setLoader("env-loader", "done");
    }).catch(function(error) {
        setLoader("env-loader", "error");
    });
}

function getHistory() {
    setLoader("history-loader", "loading");
    axios.get("/api/history").then(function(response) {
        let history = response.data.history;
        let temp = [], pressure = [], humidity = [], gas = [];
        for (let entry of history) {
            let time = new Date(entry.time * 1000);
            entry.time = time;
            temp.push({ x: time, y: entry.temp });
            pressure.push({ x: time, y: entry.pressure });
            humidity.push({ x: time, y: entry.humidity });
            gas.push({ x: time, y: entry.gas });
        }
        chart.data.datasets[0].data = temp;
        chart.data.datasets[1].data = pressure;
        chart.data.datasets[2].data = humidity;
        chart.data.datasets[3].data = gas;
        chart.update();
        vueApp.history = history;
        setLoader("history-loader", "done");
    }).catch(function(error) {
        setLoader("history-loader", "error");
    });
}

function init() {
    vueApp = new Vue({
        el: '#app',
        data: {
            properties: [{ name: "Loading...", value: "Loading..." }],
            env: { temp: 0, pressure: 0, humidity: 0, gas: 0, time: new Date() },
            history: [],
            serverConfig: { historyInterval: 10, standard: true, sleepOnReset: false },
            localConfig: { historyInterval: 10, sleepOnReset: false },
            historyIntervalOptions: [
                { name: "10 s", value: 10 },
                { name: "1 min", value: 60 },
                { name: "10 min", value: 600 },
                { name: "1 h", value: 3600 }
            ],
            showTable: false,
            activeTab: "env"
        },
        computed: {
            historyIntervalChanged: function() {
                return this.serverConfig.historyInterval !== this.localConfig.historyInterval;
            },
            sleepOnResetChanged: function() {
                return this.serverConfig.sleepOnReset !== this.localConfig.sleepOnReset;
            },
            configChanged: function() {
                return this.historyIntervalChanged || this.sleepOnResetChanged;
            }
        }
    });
    chart = new Chart(document.getElementById("chart"), {
        type: 'line',
        data: {
            datasets: [
            {
                data: [],
                label: "Temperatur [\xb0C]",
                yAxisID: "temp",
                radius: 0,
                borderColor: 'Red',
                backgroundColor: 'rgba(255,0,0,0.1)',
                lineTension: 0,
                fill: false
            },
            {
                data: [],
                label: "Luftdruck [mbar]",
                yAxisID: "pressure",
                radius: 0,
                borderColor: 'Black',
                backgroundColor: 'rgba(0,0,0,0.1)',
                lineTension: 0,
                fill: false
            },
            {
                data: [],
                label: "Feuchte [%]",
                yAxisID: "humidity",
                radius: 0,
                borderColor: 'Blue',
                backgroundColor: 'rgba(0,0,255,0.1)',
                lineTension: 0,
                fill: false
            },
            {
                data: [],
                label: "Luftqualität",
                yAxisID: "gas",
                radius: 0,
                borderColor: 'Green',
                backgroundColor: 'rgba(0,255,0,0.1)',
                lineTension: 0,
                fill: false
            },]
        },
        options: {
            maintainAspectRatio: false,
            legend: { display: true },
            scales: {
                xAxes: [{
                    type: 'time'
                }],
                yAxes: [{
                    id: "temp",
                    position: "left",
                    ticks: {
                        stepSize: 10,
                        suggestedMax: 40,
                        suggestedMin: -10,
                        fontColor: "Red"
                    }
                },
                {
                    id: "pressure",
                    position: "left",
                    ticks: {
                        stepSize: 50,
                        suggestedMax: 1200,
                        suggestedMin: 950,
                        fontColor: "Black"
                    }
                },
                {
                    id: "humidity",
                    position: "right",
                    ticks: {
                        stepSize: 10,
                        suggestedMin: 0,
                        suggestedMax: 100,
                        fontColor: "Blue"
                    }
                },
                {
                    id: "gas",
                    position: "right",
                    ticks: {
                        stepSize: 100,
                        suggestedMin: 0,
                        suggestedMax: 500,
                        fontColor: "Green"
                    }
                }]
            }
        }
    });
    getStatus(); // obtain IP address and host name
    var frag = window.location.hash.substring(1);
    if (frag) {
        openTab(frag);
    } else {
        openTab("env");
    }
}

