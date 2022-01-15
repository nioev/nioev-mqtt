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

</script>

<main>
    <div id="blueBar"></div>
    <div id="innerMain">
        <h1>
            nioev live dashboard
        </h1>
        <nav>
            {#each navElements as e}
                <p class="element" style:font-weight={currentPage === e.href ? "bold" : ""}
                   on:click={event => {currentPage = e.href; history.pushState("", "", e.href)}}>
                    {e.text}
                </p>
            {/each}
        </nav>
        {#if currentPage === "index.html"}
            <div class="content" transition:fade="{{duration: 50}}">
                <Overview/>
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
        grid-row-start: 3;
        grid-row-end: 3;
        grid-column-start: 1;
        grid-column-end: 1;
    }
    h1 {
        margin: 0;
        color: white;
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
        grid-template-rows: 50px 50px auto;
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
        font-size: 20px;
        color: black;
    }
    .element:hover {
        cursor: pointer;
    }
</style>