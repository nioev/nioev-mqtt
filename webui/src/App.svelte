<script>
    import { fade } from 'svelte/transition';
    import Overview from "./Overview.svelte";
    import Scripts from "./Scripts.svelte";
    import Settings from "./Settings.svelte";

    let currentPage = window.location.pathname.split("/").pop();

    let navElements = [{
        href: "index.html",
        text: "Overview"
    }, {
        href: "scripts.html",
        text: "Scripts"
    }, {
        href: "settings.html",
        text: "Settings"
    }]
    let refreshRate = "15000";
</script>

<main>
    <div id="blueBar"></div>
    <div id="innerMain">
        <div id="headline">
            <h1>nioev live dashboard</h1>
            <span id="updateEvery">
                <span id="updateEveryText">Update every</span>
                <select bind:value={refreshRate}>
                    <option value="1000">1s</option>
                    <option value="5000">5s</option>
                    <option value="15000" >15s</option>
                    <option value="60000">60s</option>
                </select>
            </span>
        </div>
        <nav>
            {#each navElements as e}
                <p class="element" style:font-weight={currentPage === e.href ? "bold" : ""}
                   on:click={event => {currentPage = e.href; history.pushState("", "", e.href)}}>
                    {e.text}
                </p>
            {/each}
        </nav>
        <div id="spacer"></div>
        {#if currentPage === "index.html"}
            <div class="content" transition:fade="{{duration: 50}}">
                <Overview refreshRate={refreshRate}/>
            </div>
        {:else if currentPage === "scripts.html"}
            <div class="content" transition:fade="{{duration: 50}}">
                <Scripts/>
            </div>
        {:else if currentPage === "settings.html"}
            <div class="content" transition:fade="{{duration: 50}}">
                <Settings/>
            </div>
        {:else}
            Error!
        {/if}
    </div>
</main>

<style>
    .content {
        grid-row-start: 4;
        grid-row-end: 4;
        grid-column-start: 1;
        grid-column-end: 1;
    }
    h1 {
        margin: 0;
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
    }
    #headline {
        display: flex;
        justify-content: space-between;
        color: white;
        font-size: var(--medium-font-size);
        overflow: hidden;
        text-align: center;
    }
    #updateEvery {
        display: flex;
        align-items: center;
    }
    #updateEveryText {
        text-align: center;
        padding-right: 10px;
        padding-bottom: 10px;
    }
    #blueBar {
        background-color: #2980b9;
        height: 150px;
        transform: translateY(-20px);
        z-index: -1;
        width: 100%;
    }
    main {
        width: 100%;
        margin: 0;
        height: 100px;
        display: grid;
        grid-template-rows: 50px 100px;
        justify-items: center;
    }
    nav {
        display: flex;
        flex-direction: row;
        background-color: white;
        border-radius: 10px;
        box-shadow: #333333 0 0 5px;
        align-items: center;
        grid-row-start: 2;
        grid-row-end: 2;
        grid-column-start: 1;
        grid-column-end: 1;
    }
    #innerMain {
        display: grid;
        grid-template-columns: 100%;
        grid-template-rows: 50px 50px 50px auto;
        width: 1400px;
    }
    @media only screen and (max-width: 1556px) {
        #innerMain {
            width: 90%;
        }
    }
    .element {
        text-align: center;
        padding-left: 20px;
        padding-right: 20px;
        font-size: var(--medium-font-size);
        color: black;
    }
    .element:hover {
        cursor: pointer;
    }
</style>